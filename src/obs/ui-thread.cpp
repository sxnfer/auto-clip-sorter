// Clip Atlas — getting back onto OBS's UI thread.
//
// A task is a heap-allocated std::function that obs_queue_task() carries as a void *. The trampoline
// below frees it whenever OBS runs it, including when the task is dropped during shutdown. A task still
// queued when the process exits never runs, so its payload is not freed. That is a small leak at exit.
#include "ui-thread.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <atomic>
#include <exception>
#include <memory>
#include <utility>

namespace clip_atlas {

namespace {

// Set once by ui_tasks_shutdown(). Checked when queuing, so nothing new gets queued, and again when
// running, so tasks queued before shutdown are dropped without touching plugin state.
std::atomic<bool> g_shutdown{false};

// The heap object that outlives run_on_ui(). Ownership passes to the trampoline when OBS runs it.
struct QueuedTask {
	std::function<void()> fn;
};

// Runs on OBS's UI thread. The unique_ptr takes ownership first, so the payload is freed even when the
// task is dropped or fn throws.
void run_queued_task(void *param)
{
	std::unique_ptr<QueuedTask> task(static_cast<QueuedTask *>(param));
	if (g_shutdown.load(std::memory_order_acquire))
		return;
	try {
		task->fn();
	} catch (const std::exception &e) {
		obs_log(LOG_ERROR, "UI task failed: %s", e.what());
	} catch (...) {
		obs_log(LOG_ERROR, "UI task failed: unknown exception");
	}
}

} // namespace

void run_on_ui(std::function<void()> fn)
{
	if (!fn || g_shutdown.load(std::memory_order_acquire))
		return;

	// std::bad_alloc is the only exception that can leave this function.
	std::unique_ptr<QueuedTask> task(new QueuedTask{std::move(fn)});
	// wait is always false, because waiting on the UI thread would deadlock. OBS's UI handler calls the
	// task with Qt::AutoConnection, so when this is called on the UI thread the task runs before
	// obs_queue_task() returns. Calls from other threads are deferred to the event loop.
	obs_queue_task(OBS_TASK_UI, run_queued_task, task.release(), false);
}

bool is_ui_thread()
{
	return obs_in_task_thread(OBS_TASK_UI);
}

void ui_tasks_shutdown()
{
	g_shutdown.store(true, std::memory_order_release);
}

} // namespace clip_atlas
