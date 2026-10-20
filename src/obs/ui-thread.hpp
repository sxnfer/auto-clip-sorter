// Clip Atlas — getting back onto OBS's UI thread.
//
// OBS source signals fire on the graphics thread and file-mover callbacks on
// the mover's worker thread. Plugin state and every obs_frontend_* call live
// on the UI thread, so anything coming from elsewhere hops here first.
//
// Built on obs_queue_task(OBS_TASK_UI, task, param, false) from OBS 31's
// obs.h; obs_in_task_thread(OBS_TASK_UI) answers is_ui_thread().
#pragma once

#include <functional>

namespace clip_atlas {

// Runs `fn` on the UI thread. Called from another thread, it is queued and
// runs later. Called on the UI thread, it runs before run_on_ui() returns:
// OBS's UI task handler uses Qt::AutoConnection, which calls directly on the
// same thread. Callers on the UI thread must not rely on it being deferred.
// Safe from any thread. An exception thrown by `fn` is caught and logged.
// After ui_tasks_shutdown() new and still-queued tasks are dropped without
// running.
void run_on_ui(std::function<void()> fn);

// True on OBS's UI thread.
bool is_ui_thread();

// Called first thing in obs_module_unload(): stops queued tasks from running
// against plugin state that is being torn down.
void ui_tasks_shutdown();

} // namespace clip_atlas
