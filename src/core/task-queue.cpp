#include "task-queue.hpp"

#include <utility>

namespace clip_atlas {

namespace {

// Tasks and hooks report their own errors; one that throws anyway must not end the thread, or every later task
// would be lost.
void run_guarded(const std::function<void()> &fn)
{
	if (!fn)
		return;
	try {
		fn();
	} catch (...) {
	}
}

} // namespace

TaskQueue::TaskQueue(Hook on_thread_start, Hook on_thread_exit)
{
	thread_ = std::thread(&TaskQueue::run, this, std::move(on_thread_start), std::move(on_thread_exit));
}

TaskQueue::~TaskQueue()
{
	shutdown();
	// Destroyed by its own task: joining would deadlock and a joinable std::thread would terminate the process.
	if (thread_.joinable())
		thread_.detach();
}

bool TaskQueue::post(Task task)
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (stopping_)
			return false;
		tasks_.push_back(std::move(task));
	}
	wake_.notify_one();
	return true;
}

std::size_t TaskQueue::clear()
{
	std::deque<Task> dropped;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		dropped.swap(tasks_);
	}
	// Destroyed outside the lock: a task's captures may run code of their own when they are destroyed.
	return dropped.size();
}

void TaskQueue::shutdown()
{
	std::deque<Task> dropped;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		stopping_ = true;
		dropped.swap(tasks_);
	}
	wake_.notify_all();
	dropped.clear();

	std::lock_guard<std::mutex> join_lock(join_mutex_);
	if (!thread_.joinable())
		return;
	// Called from a task, the thread can't join itself. It stops once the task returns, and a later shutdown() from
	// another thread (the destructor's, usually) joins it.
	if (thread_.get_id() == std::this_thread::get_id())
		return;
	thread_.join();
}

bool TaskQueue::on_queue_thread() const
{
	return thread_.get_id() == std::this_thread::get_id();
}

void TaskQueue::run(Hook on_thread_start, Hook on_thread_exit)
{
	run_guarded(on_thread_start);
	for (;;) {
		Task task;
		{
			std::unique_lock<std::mutex> lock(mutex_);
			wake_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
			if (stopping_)
				break;
			task = std::move(tasks_.front());
			tasks_.pop_front();
		}
		run_guarded(task);
	}
	run_guarded(on_thread_exit);
}

} // namespace clip_atlas
