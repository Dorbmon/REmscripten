/*
 * Copyright 2021 The Emscripten Authors.  All rights reserved.
 * Emscripten is available under two separate licenses, the MIT license and the
 * University of Illinois/NCSA Open Source License.  Both these licenses can be
 * found in the LICENSE file.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <errno.h>
#include <functional>
#include <memory>
#include <thread>

#include <emscripten/proxying.h>
#include <emscripten/threading.h>

extern "C" {
void _wasmfs_thread_utils_heartbeat(em_proxying_queue* ctx);

// These are deliberately private C imports rather than declarations from the
// C-only pthread internals header. The browser-main implementation fences one
// Worker out of PThread.unusedWorkers and reports when cleanupThread has
// actually requested its termination.
int _emscripten_thread_retire_worker(pthread_t thread, uint32_t ticket);
int _emscripten_thread_retire_worker_status(uint32_t ticket);
}

namespace emscripten {

// Helper class for synchronously proxying work to a dedicated worker thread,
// including where the work is asynchronous.
class ProxyWorker {
  // The queue we use to proxy work and the dedicated worker.
  std::unique_ptr<ProxyingQueue> queue;

  // Used to notify the calling thread once the worker has been started.
  bool started = false;
  std::mutex mutex;
  std::condition_variable cond;
  // Declare the thread last since it's dependent on the above member variables.
  // Declaring it last isn't strictly needed since the thread is initialized in
  // the body of the constructor, but is done out of caution.
  std::thread thread;

  // A scoped OPFS profile handoff fences this Worker on the browser main
  // thread before releasing its Web Lock. The ticket has browser-main
  // lifetime, so an asynchronous cleanup acknowledgement can never write into
  // an already-destroyed native ProxyWorker.
  uint32_t retirementTicket = 0;
  bool retirementFenced = false;
  bool heartbeatStopped = false;
  bool retired = false;

  static int normalizeError(int error) {
    return error > 0 ? -error : error;
  }

  int waitForRetirementCleanup() {
    // pthread_join waits for the target's C pthread state, but its JS
    // cleanupThread message is asynchronous. Poll the browser-main ticket
    // from the calling application pthread until it confirms that the exact
    // Worker was terminated rather than placed in unusedWorkers.
    constexpr int kRetries = 5000;
    for (int i = 0; i != kRetries; ++i) {
      if (_emscripten_thread_retire_worker_status(retirementTicket)) {
        return 0;
      }
      emscripten_thread_sleep(1);
    }
    return -ETIMEDOUT;
  }

public:
  // Spawn the worker thread.
  ProxyWorker() : queue(std::make_unique<ProxyingQueue>()) {
    // Initialize the thread in the constructor to ensure the object has been
    // fully constructed before thread starts using the object to avoid a data
    // race. See #24370.
    // Capture the raw queue before publishing worker startup. A failed scoped
    // OPFS handoff can then detach and intentionally release the unique_ptr
    // while this live Worker keeps its heartbeat; it must never race a later
    // `queue->queue` member dereference in this lambda.
    auto* rawQueue = queue->queue;
    thread = std::thread([&, rawQueue]() {
      // Sometimes the main thread is spinning, waiting on a WasmFS lock held
      // by a thread trying to proxy work to this dedicated worker. In that
      // case, the proxying message won't be relayed by the main thread and
      // the system will deadlock. This heartbeat ensures that proxying work
      // eventually gets done so the thread holding the lock can make forward
      // progress even if the main thread is blocked.
      //
      // TODO: Remove this once we can postMessage directly between workers
      // without involving the main thread or once all browsers ship
      // Atomics.waitAsync.
      //
      // Note that this requires adding _emscripten_proxy_execute_queue to
      // EXPORTED_FUNCTIONS.
      _wasmfs_thread_utils_heartbeat(rawQueue);

      // Publish startup only after the heartbeat owns the captured queue.
      // This makes the scoped failed-fence abandon path safe even if it runs
      // immediately after the constructor returns.
      {
        std::unique_lock<std::mutex> lock(mutex);
        started = true;
      }
      cond.notify_all();

      // Sit in the event loop performing work as it comes in.
      emscripten_exit_with_live_runtime();
    });

    // Make sure the thread has actually started before returning. This allows
    // subsequent code to assume the thread has already been spawned and not
    // worry about potential deadlocks where it holds a lock while proxying an
    // operation and meanwhile the main thread is blocked trying to acquire the
    // same lock so is never able to spawn the worker thread.
    //
    // Unfortunately, this solution would cause the main thread to deadlock on
    // itself, so for now assert that we are not on the main thread. In the
    // future, we could provide an asynchronous version of this utility that
    // calls a user callback once the worker has been started. This asynchronous
    // version would be safe to use on the main thread.
    assert(
      !emscripten_is_main_browser_thread() &&
      "cannot safely spawn dedicated workers from the main browser thread");
    {
      std::unique_lock<std::mutex> lock(mutex);
      cond.wait(lock, [&]() { return started; });
    }
  }

  // Kill the worker thread.
  ~ProxyWorker() {
    // A successful scoped retirement has already joined the dedicated
    // pthread on an application Worker. In particular, never perform a
    // second join from WasmFS global destruction on the browser main thread.
    if (thread.joinable()) {
      // A failed scoped handoff should normally have abandoned this worker on
      // the application pthread already. Keep this defensive browser-main
      // path non-blocking as well: its queue remains allocated for the live
      // heartbeat until the document tears down the fenced Worker.
      if (retirementFenced && emscripten_is_main_browser_thread()) {
        queue.release();
        thread.detach();
        return;
      }
      pthread_cancel(thread.native_handle());
      thread.join();
    }
  }

  // Proxy synchronous work.
  bool operator()(const std::function<void()>& func) {
    return queue->proxySync(thread.native_handle(), func);
  }
  // Proxy asynchronous work that calls `finish()` on the ctx parameter to mark
  // its end.
  bool operator()(const std::function<void(ProxyingQueue::ProxyingCtx)>& func) {
    return queue->proxySyncWithCtx(thread.native_handle(), func);
  }

  em_proxying_queue* getQueue() const { return queue->queue; }

  // Fence this exact browser Worker before a scoped profile drain releases
  // its Web Lock. A later failure may retain a tombstoned backend, but cannot
  // let its stateful JavaScript realm become a generic pthread-pool worker.
  int fenceForRetirement() {
    if (retirementFenced) {
      return 0;
    }
    if (!thread.joinable()) {
      return -ESHUTDOWN;
    }
    static std::atomic<uint32_t> nextRetirementTicket{1};
    uint32_t ticket = nextRetirementTicket.fetch_add(1);
    if (ticket == 0) {
      ticket = nextRetirementTicket.fetch_add(1);
    }
    int error = _emscripten_thread_retire_worker(thread.native_handle(), ticket);
    if (error) {
      return normalizeError(error);
    }
    retirementTicket = ticket;
    retirementFenced = true;
    return 0;
  }

  // The scoped OPFS transaction clears the JavaScript interval in the same
  // callback that acknowledges Web Locks release. Publish that acknowledgement
  // to native retirement only after proxySyncWithCtx has returned.
  void markHeartbeatStopped() {
    assert(retirementFenced);
    heartbeatStopped = true;
  }

  // Retain the queue deliberately when a scoped profile handoff fails before
  // it can stop the heartbeat. This also covers a failed browser-main fence:
  // in that case the still-running Worker cannot be pooled because we do not
  // cancel it, and the sealed backend retains its lease until document
  // teardown. The native detach happens away from browser main, so a later
  // global WasmFS destructor cannot fall back to pthread_join there. This
  // method is OPFS-profile-specific; ordinary ProxyWorker users retain their
  // existing destructor behavior.
  void abandonScopedProfileWorker() {
    if (!thread.joinable()) {
      return;
    }
    queue.release();
    thread.detach();
  }

  // Retire a fenced Worker on the application pthread. This is deliberately
  // terminal even if a post-release cleanup step reports an error: after the
  // Web Lock is released, returning with a joinable ProxyWorker would defer a
  // blocking join to global WasmFS destruction on the browser main thread.
  int retire() {
    if (retired) {
      return 0;
    }
    if (!retirementFenced || !heartbeatStopped || !thread.joinable()) {
      return -ESHUTDOWN;
    }

    int firstError = 0;
    // The acknowledged scoped transaction released the lock, reset OPFS
    // worker state, and synchronously stopped the heartbeat before returning
    // to this application pthread. Always attempt cancellation here so
    // post-release cleanup cannot defer a join to global WasmFS destruction
    // on the browser main thread.
    int cancelError = pthread_cancel(thread.native_handle());
    if (cancelError) {
      if (!firstError) {
        firstError = -cancelError;
      }
      // The Worker was fenced before release and its heartbeat has already
      // been stopped. Detach and retain its queue so this error path cannot
      // fall back to a browser-main join or invalidate a late worker callback.
      abandonScopedProfileWorker();
      return firstError;
    }

    thread.join();

    if (int cleanupError = waitForRetirementCleanup()) {
      if (!firstError) {
        firstError = cleanupError;
      }
      // Do not terminate the browser Worker directly here: that could consume
      // its pending cleanupThread message and leak PThread's bookkeeping. The
      // native thread is already non-joinable, the heartbeat is stopped, and
      // the pre-release fence makes eventual normal cleanup terminate rather
      // than pool this worker. Only that normal acknowledgement permits
      // backend_retired to be reported.
    }

    retired = true;
    return firstError;
  }
};

} // namespace emscripten
