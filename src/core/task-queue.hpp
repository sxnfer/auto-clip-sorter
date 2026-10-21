// Clip Atlas — one background thread that runs tasks in order.
//
// The dock scans the clips folder and the Shell code makes thumbnails and
// recycles files on threads of their own, so the UI thread never touches the
// disk. Each owner gets one TaskQueue. The Shell code needs COM initialised on
// the thread that calls it, so the queue runs an optional hook when its thread
// starts and another just before it ends.
//
// Pure C++17 with no OBS dependency.
#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace clip_atlas {

class TaskQueue {
public:
	using Task = std::function<void()>;
	using Hook = std::function<void()>;

	// Starts the thread. `on_thread_start` runs on it before the first task,
	// `on_thread_exit` after the last; either may be empty. An exception from a
	// hook or a task is caught and dropped (the owner reports errors through its
	// own callbacks), and the thread carries on.
	explicit TaskQueue(Hook on_thread_start = {}, Hook on_thread_exit = {});
	// Calls shutdown().
	~TaskQueue();

	TaskQueue(const TaskQueue &) = delete;
	TaskQueue &operator=(const TaskQueue &) = delete;

	// Queues `task` at the back. Returns false, and drops the task without
	// running it, after shutdown(). Safe from any thread, including from a task.
	bool post(Task task);

	// Drops every queued task without running it. A task already running
	// finishes. Returns how many were dropped. Safe from any thread.
	std::size_t clear();

	// Drops the queued tasks, waits for the running one, runs on_thread_exit and
	// joins the thread. Idempotent and safe from any thread. Called from one of
	// its own tasks it can't join: it stops the queue and returns, and the
	// thread ends once that task returns (a later shutdown() from another thread
	// joins it). Never destroy the queue from its own task.
	void shutdown();

	// True on this queue's thread. Not for use during shutdown() from another
	// thread.
	bool on_queue_thread() const;

private:
	void run(Hook on_thread_start, Hook on_thread_exit);

	std::mutex mutex_;
	// Serializes the join in shutdown(). Kept apart from mutex_, which the thread
	// needs to finish.
	std::mutex join_mutex_;
	std::condition_variable wake_;
	std::deque<Task> tasks_;
	bool stopping_ = false;
	std::thread thread_;
};

} // namespace clip_atlas
