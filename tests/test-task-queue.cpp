#include "task-queue.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace clip_atlas;
using namespace std::chrono_literals;

TEST_CASE("TaskQueue runs tasks in order on its own thread", "[task-queue]")
{
	TaskQueue queue;
	std::mutex m;
	std::vector<int> order;
	std::promise<std::thread::id> where;

	for (int i = 0; i < 5; ++i)
		REQUIRE(queue.post([&, i] {
			std::lock_guard<std::mutex> lock(m);
			order.push_back(i);
		}));
	REQUIRE(queue.post([&] { where.set_value(std::this_thread::get_id()); }));

	const std::thread::id id = where.get_future().get();
	CHECK(id != std::this_thread::get_id());
	std::lock_guard<std::mutex> lock(m);
	CHECK(order == std::vector<int>{0, 1, 2, 3, 4});
}

TEST_CASE("TaskQueue runs the start hook first and the exit hook last, on its thread", "[task-queue]")
{
	std::vector<std::string> events;
	std::mutex m;
	auto log = [&](const char *what) {
		std::lock_guard<std::mutex> lock(m);
		events.push_back(what);
	};
	std::atomic<bool> hooks_on_queue_thread{true};
	{
		TaskQueue *self = nullptr;
		std::promise<void> ready;
		TaskQueue queue([&] { log("start"); }, [&] { log("exit"); });
		self = &queue;
		queue.post([&] {
			log("task");
			if (!self->on_queue_thread())
				hooks_on_queue_thread = false;
			ready.set_value();
		});
		ready.get_future().wait();
		CHECK_FALSE(queue.on_queue_thread());
	}
	CHECK(hooks_on_queue_thread);
	CHECK(events == std::vector<std::string>{"start", "task", "exit"});
}

TEST_CASE("TaskQueue keeps running after a task throws", "[task-queue]")
{
	TaskQueue queue([] { throw std::runtime_error("hook"); });
	std::promise<int> after;
	queue.post([] { throw std::runtime_error("task"); });
	queue.post([] { throw 42; });
	queue.post([&] { after.set_value(7); });
	CHECK(after.get_future().get() == 7);
}

TEST_CASE("TaskQueue clear drops queued tasks but lets the running one finish", "[task-queue]")
{
	TaskQueue queue;
	std::mutex m;
	std::condition_variable cv;
	bool started = false;
	bool release = false;
	std::atomic<int> ran{0};

	queue.post([&] {
		std::unique_lock<std::mutex> lock(m);
		started = true;
		cv.notify_all();
		cv.wait(lock, [&] { return release; });
		++ran;
	});
	{
		std::unique_lock<std::mutex> lock(m);
		cv.wait(lock, [&] { return started; });
	}
	queue.post([&] { ++ran; });
	queue.post([&] { ++ran; });
	CHECK(queue.clear() == 2);
	{
		std::lock_guard<std::mutex> lock(m);
		release = true;
	}
	cv.notify_all();

	std::promise<void> done;
	queue.post([&] { done.set_value(); });
	done.get_future().wait();
	CHECK(ran == 1);
}

TEST_CASE("TaskQueue shutdown waits for the running task and drops the rest", "[task-queue]")
{
	TaskQueue queue;
	std::atomic<bool> first_done{false};
	std::atomic<int> later{0};
	std::promise<void> started;

	queue.post([&] {
		started.set_value();
		std::this_thread::sleep_for(50ms);
		first_done = true;
	});
	queue.post([&] { ++later; });
	started.get_future().wait();
	queue.shutdown();

	CHECK(first_done);
	CHECK(later == 0);
	CHECK_FALSE(queue.post([&] { ++later; }));
	queue.shutdown(); // idempotent
	CHECK(later == 0);
}

TEST_CASE("TaskQueue shutdown from two threads at once is safe", "[task-queue]")
{
	TaskQueue queue;
	queue.post([] { std::this_thread::sleep_for(20ms); });
	std::thread other([&] { queue.shutdown(); });
	queue.shutdown();
	other.join();
	CHECK_FALSE(queue.post([] {}));
}

TEST_CASE("TaskQueue accepts posts from its own tasks", "[task-queue]")
{
	TaskQueue queue;
	std::promise<int> inner;
	queue.post([&] { queue.post([&] { inner.set_value(3); }); });
	CHECK(inner.get_future().get() == 3);
}

TEST_CASE("TaskQueue shutdown from a task while another thread is shutting down does not deadlock", "[task-queue]")
{
	TaskQueue queue;
	std::promise<void> task_started;
	std::promise<void> release_task;
	std::shared_future<void> release = release_task.get_future().share();
	std::atomic<bool> inner_returned{false};

	queue.post([&] {
		task_started.set_value();
		release.wait();
		// The other thread is inside shutdown(), waiting to join this thread.
		queue.shutdown();
		inner_returned = true;
	});
	task_started.get_future().wait();
	std::thread other([&] { queue.shutdown(); });
	std::this_thread::sleep_for(20ms); // let `other` reach the join
	release_task.set_value();
	other.join();
	CHECK(inner_returned);
	CHECK_FALSE(queue.post([] {}));
}
