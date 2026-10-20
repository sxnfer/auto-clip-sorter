#include "clip-layout.hpp"
#include "file-mover.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <functional>
#include <future>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace clip_atlas;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

using Clock = std::chrono::steady_clock;

// How long a test waits for a callback before it fails. Generous, so a slow CI
// machine passes, while a hung worker fails the test instead of stalling the run.
constexpr std::chrono::seconds kWaitLimit{20};

// A fresh, empty folder under the system temp directory, removed when the test ends.
class TempDir {
public:
	TempDir()
	{
		std::random_device rd;
		// Two statements: the order of two rd() calls in one expression is unspecified.
		const unsigned long long hi = rd();
		const unsigned long long lo = rd();
		const unsigned long long nonce = (hi << 32) | lo;
		path_ = fs::temp_directory_path() / path_from_utf8("clip-atlas-file-mover-" + std::to_string(nonce));
		std::error_code ec;
		fs::create_directories(path_, ec);
		REQUIRE(!ec);
	}
	~TempDir()
	{
		std::error_code ec;
		fs::remove_all(path_, ec);
	}
	TempDir(const TempDir &) = delete;
	TempDir &operator=(const TempDir &) = delete;

	const fs::path &path() const { return path_; }

private:
	fs::path path_;
};

// Writes a whole file. The stream is closed before returning, because Windows
// cannot move or delete a file that is still open.
void write_file(const fs::path &p, const std::string &content)
{
	std::ofstream out(p, std::ios::binary | std::ios::trunc);
	REQUIRE(out.is_open());
	out << content;
	out.close();
	REQUIRE(!out.fail());
}

std::string read_file(const fs::path &p)
{
	std::ifstream in(p, std::ios::binary);
	return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Records every callback with the thread it ran on. Tests wait on this instead of
// sleeping, and can count how often each job reported.
template<typename Result> class Log {
public:
	struct Entry {
		Result result;
		std::thread::id thread;
		Clock::time_point when; // when the callback ran
	};

	std::function<void(const Result &)> callback()
	{
		return [this](const Result &result) {
			std::lock_guard<std::mutex> lock(mutex_);
			entries_.push_back(Entry{result, std::this_thread::get_id(), Clock::now()});
			changed_.notify_all();
		};
	}

	// Blocks until `count` callbacks have arrived, then returns everything so far.
	std::vector<Entry> wait_for(std::size_t count)
	{
		std::unique_lock<std::mutex> lock(mutex_);
		const bool arrived =
			changed_.wait_for(lock, kWaitLimit, [this, count] { return entries_.size() >= count; });
		REQUIRE(arrived);
		return entries_;
	}

	// Everything recorded so far, without waiting.
	std::vector<Entry> snapshot()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		return entries_;
	}

private:
	std::mutex mutex_;
	std::condition_variable changed_;
	std::vector<Entry> entries_;
};

using MoveLog = Log<MoveResult>;
using StableLog = Log<StableResult>;

// Short timings so the suite runs in seconds. Assertions only use bounds with slack.
FileMover::Options fast_options(std::shared_ptr<FileOps> ops = nullptr, std::chrono::milliseconds budget = 300ms)
{
	FileMover::Options options;
	options.retry_budget = budget;
	options.initial_backoff = 5ms;
	options.max_backoff = 20ms;
	options.stable_poll_interval = 10ms;
	options.file_ops = std::move(ops);
	return options;
}

// The real filesystem with injected failures. Knobs are set before a job is queued.
// The worker makes the calls, so the counters are atomic.
class FakeOps final : public FileOps {
public:
	// `watch` is a path whose first exists() or rename() call releases started().
	explicit FakeOps(fs::path watch = {}) : real_(FileMover::default_file_ops()), watch_(std::move(watch)) {}

	// Call once, before the job is queued.
	std::future<void> started() { return started_.get_future(); }

	fs::path rename_fail_from;             // renames starting here fail ...
	std::error_code rename_fail_error;     // ... with this error ...
	std::atomic<int> rename_fail_times{0}; // ... this many times, or forever when -1
	fs::path remove_fail_path;             // removes of this path always fail
	std::atomic<bool> fail_create_directories{false};
	fs::path size_lie_path; // file_size() of this path is one byte too big
	std::atomic<int> rename_calls{0};
	// When set, returns an error for the renames it picks (empty error: the real rename runs).
	std::function<std::error_code(const fs::path &, const fs::path &)> rename_override;
	fs::path throw_on_rename_from;                                         // rename() from here throws
	std::function<void(const fs::path &, const fs::path &)> before_rename; // runs before each rename
	std::function<void(const fs::path &, const fs::path &)> after_copy;    // runs after each good copy
	std::chrono::milliseconds copy_delay{0};                               // copy_file sleeps first
	std::atomic<int> copy_fail_times{0};   // copies that write junk and then fail, this many times
	fs::path remove_busy_path;             // removes of this path are refused ...
	std::atomic<int> remove_busy_times{0}; // ... this many times

	bool exists(const fs::path &p) override
	{
		note_watched(p);
		return real_->exists(p);
	}

	std::error_code create_directories(const fs::path &dir) override
	{
		if (fail_create_directories.load())
			return std::make_error_code(std::errc::permission_denied);
		return real_->create_directories(dir);
	}

	std::error_code rename(const fs::path &from, const fs::path &to) override
	{
		++rename_calls;
		note_watched(from);
		if (before_rename)
			before_rename(from, to);
		if (!throw_on_rename_from.empty() && from == throw_on_rename_from)
			throw std::runtime_error("injected: rename threw");
		if (rename_override) {
			const std::error_code injected = rename_override(from, to);
			if (injected)
				return injected;
		}
		if (!rename_fail_from.empty() && from == rename_fail_from && rename_fail_times.load() != 0) {
			if (rename_fail_times.load() > 0)
				--rename_fail_times;
			return rename_fail_error;
		}
		return real_->rename(from, to);
	}

	std::error_code copy_file(const fs::path &from, const fs::path &to) override
	{
		if (copy_delay.count() > 0)
			std::this_thread::sleep_for(copy_delay);
		if (copy_fail_times.load() > 0) {
			--copy_fail_times;
			std::ofstream junk(to, std::ios::binary | std::ios::trunc);
			junk << "junk";
			junk.close();
			return std::make_error_code(std::errc::no_space_on_device);
		}
		const std::error_code ec = real_->copy_file(from, to);
		if (!ec && after_copy)
			after_copy(from, to);
		return ec;
	}

	std::error_code remove(const fs::path &p) override
	{
		if (!remove_fail_path.empty() && p == remove_fail_path)
			return std::make_error_code(std::errc::permission_denied);
		if (!remove_busy_path.empty() && p == remove_busy_path && remove_busy_times.load() > 0) {
			--remove_busy_times;
			return std::make_error_code(std::errc::device_or_resource_busy);
		}
		return real_->remove(p);
	}

	std::optional<std::uintmax_t> file_size(const fs::path &p) override
	{
		std::optional<std::uintmax_t> size = real_->file_size(p);
		if (size && !size_lie_path.empty() && p == size_lie_path)
			return *size + 1;
		return size;
	}

	std::optional<fs::file_time_type> last_write_time(const fs::path &p) override
	{
		return real_->last_write_time(p);
	}

private:
	void note_watched(const fs::path &p)
	{
		if (!watch_.empty() && p == watch_ && !signalled_.exchange(true))
			started_.set_value();
	}

	std::shared_ptr<FileOps> real_;
	fs::path watch_;
	std::atomic<bool> signalled_{false};
	std::promise<void> started_;
};

} // namespace

TEST_CASE("move puts the file in a new nested folder", "[file-mover]")
{
	TempDir tmp;
	MoveLog log;
	FileMover mover(fast_options());
	const fs::path source = tmp.path() / "Replay 2026-10-20.mkv";
	const fs::path dest_dir = tmp.path() / "Game" / "2026-10";
	write_file(source, "clip-bytes");

	mover.move(source, dest_dir, log.callback());
	const auto entries = log.wait_for(1);
	REQUIRE(entries.size() == 1);
	const MoveResult &r = entries[0].result;
	CHECK(r.id == 1);
	CHECK(r.status == MoveStatus::moved);
	CHECK(r.final_path == dest_dir / "Replay 2026-10-20.mkv");
	CHECK(r.error.empty());
	CHECK(!fs::exists(source));
	CHECK(read_file(r.final_path) == "clip-bytes");
	// Callbacks run on the worker, never on the thread that queued the job.
	CHECK(entries[0].thread != std::this_thread::get_id());
}

TEST_CASE("a name collision gets a numbered name and leaves the existing file alone", "[file-mover]")
{
	TempDir tmp;
	MoveLog log;
	FileMover mover(fast_options());
	const fs::path dest_dir = tmp.path() / "Game";
	fs::create_directories(dest_dir);
	write_file(dest_dir / "Replay.mkv", "existing");
	const fs::path source = tmp.path() / "Replay.mkv";
	write_file(source, "incoming");

	mover.move(source, dest_dir, log.callback());
	const auto entries = log.wait_for(1);
	const MoveResult &r = entries[0].result;
	CHECK(r.status == MoveStatus::moved);
	CHECK(r.final_path == dest_dir / "Replay (2).mkv");
	CHECK(read_file(dest_dir / "Replay.mkv") == "existing");
	CHECK(read_file(r.final_path) == "incoming");
	CHECK(!fs::exists(source));
}

TEST_CASE("a missing source fails at once with no retry", "[file-mover]")
{
	TempDir tmp;
	MoveLog log;
	auto ops = std::make_shared<FakeOps>();
	// A long budget: if the job retried, the wait below would take seconds.
	FileMover mover(fast_options(ops, 5000ms));
	const auto started = Clock::now();

	mover.move(tmp.path() / "gone.mkv", tmp.path() / "Game", log.callback());
	const auto entries = log.wait_for(1);
	CHECK(Clock::now() - started < 2s);
	const MoveResult &r = entries[0].result;
	CHECK(r.status == MoveStatus::failed);
	CHECK(r.error == "source missing");
	CHECK(ops->rename_calls.load() == 0);
	CHECK(!fs::exists(tmp.path() / "Game"));
}

TEST_CASE("a locked source is retried and moves once it frees up", "[file-mover]")
{
	TempDir tmp;
	MoveLog log;
	auto ops = std::make_shared<FakeOps>();
	const fs::path source = tmp.path() / "locked.mkv";
	const fs::path dest_dir = tmp.path() / "Game";
	write_file(source, "data");
	ops->rename_fail_from = source;
	ops->rename_fail_error = std::make_error_code(std::errc::permission_denied);
	ops->rename_fail_times = 3;
	// A generous budget: this test is about the retries, not about the clock.
	FileMover mover(fast_options(ops, 5000ms));

	mover.move(source, dest_dir, log.callback());
	const auto entries = log.wait_for(1);
	const MoveResult &r = entries[0].result;
	CHECK(r.status == MoveStatus::moved);
	CHECK(r.final_path == dest_dir / "locked.mkv");
	CHECK(ops->rename_calls.load() == 4); // three refusals, then the rename that works
	CHECK(read_file(r.final_path) == "data");
	CHECK(!fs::exists(source));
}

TEST_CASE("a source that stays locked fails after the retry budget and is left alone", "[file-mover]")
{
	TempDir tmp;
	MoveLog log;
	auto ops = std::make_shared<FakeOps>();
	const fs::path source = tmp.path() / "locked.mkv";
	const fs::path dest_dir = tmp.path() / "Game";
	write_file(source, "still-here");
	ops->rename_fail_from = source;
	ops->rename_fail_error = std::make_error_code(std::errc::permission_denied);
	ops->rename_fail_times = -1;
	const std::chrono::milliseconds budget = 300ms;
	FileMover mover(fast_options(ops, budget));

	const auto started = Clock::now();
	mover.move(source, dest_dir, log.callback());
	const auto entries = log.wait_for(1);
	const auto elapsed = Clock::now() - started;

	const MoveResult &r = entries[0].result;
	CHECK(r.status == MoveStatus::failed);
	CHECK(!r.error.empty());
	CHECK(r.final_path.empty());
	// The budget is a floor: the job keeps trying until it is spent.
	CHECK(elapsed >= budget);
	CHECK(elapsed < budget + 5s);
	CHECK(ops->rename_calls.load() > 1);
	CHECK(read_file(source) == "still-here");
	CHECK(!fs::exists(dest_dir / "locked.mkv"));
}

TEST_CASE("across volumes the copy is verified and then the source is removed", "[file-mover]")
{
	TempDir tmp;
	MoveLog log;
	auto ops = std::make_shared<FakeOps>();
	const fs::path source = tmp.path() / "xdev.mkv";
	const fs::path dest_dir = tmp.path() / "Game";
	const std::string content = std::string(64 * 1024, 'x') + "tail";
	write_file(source, content);
	ops->rename_fail_from = source;
	ops->rename_fail_error = std::make_error_code(std::errc::cross_device_link);
	ops->rename_fail_times = -1;
	FileMover mover(fast_options(ops));

	mover.move(source, dest_dir, log.callback());
	const auto entries = log.wait_for(1);
	const MoveResult &r = entries[0].result;
	CHECK(r.status == MoveStatus::moved);
	CHECK(r.final_path == dest_dir / "xdev.mkv");
	CHECK(read_file(r.final_path) == content);
	CHECK(!fs::exists(source));
	CHECK(!fs::exists(dest_dir / "xdev.mkv.clip-atlas-partial"));
	// One refused rename of the source, then the rename of the verified copy.
	CHECK(ops->rename_calls.load() == 2);
}

TEST_CASE("a copy avoids a partial name that another file already holds", "[file-mover]")
{
	TempDir tmp;
	MoveLog log;
	auto ops = std::make_shared<FakeOps>();
	const fs::path source = tmp.path() / "x.mkv";
	const fs::path dest_dir = tmp.path() / "Game";
	fs::create_directories(dest_dir);
	write_file(dest_dir / "x.mkv.clip-atlas-partial", "someone else's");
	write_file(source, "payload");
	ops->rename_fail_from = source;
	ops->rename_fail_error = std::make_error_code(std::errc::cross_device_link);
	ops->rename_fail_times = -1;
	FileMover mover(fast_options(ops));

	mover.move(source, dest_dir, log.callback());
	const auto entries = log.wait_for(1);
	const MoveResult &r = entries[0].result;
	CHECK(r.status == MoveStatus::moved);
	CHECK(r.final_path == dest_dir / "x (2).mkv");
	CHECK(read_file(r.final_path) == "payload");
	// The partial name belongs to someone else, so it must survive untouched.
	CHECK(read_file(dest_dir / "x.mkv.clip-atlas-partial") == "someone else's");
	CHECK(!fs::exists(source));
}

TEST_CASE("a copy whose size does not match is removed and the source is kept", "[file-mover]")
{
	TempDir tmp;
	MoveLog log;
	auto ops = std::make_shared<FakeOps>();
	const fs::path source = tmp.path() / "bad.mkv";
	const fs::path dest_dir = tmp.path() / "Game";
	write_file(source, "payload");
	ops->rename_fail_from = source;
	ops->rename_fail_error = std::make_error_code(std::errc::cross_device_link);
	ops->rename_fail_times = -1;
	ops->size_lie_path = dest_dir / "bad.mkv.clip-atlas-partial";
	FileMover mover(fast_options(ops));

	mover.move(source, dest_dir, log.callback());
	const auto entries = log.wait_for(1);
	const MoveResult &r = entries[0].result;
	CHECK(r.status == MoveStatus::failed);
	CHECK(r.error == "copy size mismatch");
	CHECK(r.final_path.empty());
	CHECK(read_file(source) == "payload");
	CHECK(!fs::exists(dest_dir / "bad.mkv.clip-atlas-partial"));
	CHECK(!fs::exists(dest_dir / "bad.mkv"));
}

TEST_CASE("a source that cannot be removed after a verified copy keeps both files", "[file-mover]")
{
	TempDir tmp;
	MoveLog log;
	auto ops = std::make_shared<FakeOps>();
	const fs::path source = tmp.path() / "keep.mkv";
	const fs::path dest_dir = tmp.path() / "Game";
	write_file(source, "keep-me");
	ops->rename_fail_from = source;
	ops->rename_fail_error = std::make_error_code(std::errc::cross_device_link);
	ops->rename_fail_times = -1;
	ops->remove_fail_path = source;
	FileMover mover(fast_options(ops, 200ms));

	mover.move(source, dest_dir, log.callback());
	const auto entries = log.wait_for(1);
	const MoveResult &r = entries[0].result;
	CHECK(r.status == MoveStatus::copied_source_kept);
	CHECK(r.final_path == dest_dir / "keep.mkv");
	CHECK(!r.error.empty());
	CHECK(read_file(source) == "keep-me");
	CHECK(read_file(r.final_path) == "keep-me");
}

TEST_CASE("an uncreatable destination fails after the budget and leaves the source", "[file-mover]")
{
	TempDir tmp;
	MoveLog log;
	auto ops = std::make_shared<FakeOps>();
	const fs::path source = tmp.path() / "stuck.mkv";
	const fs::path dest_dir = tmp.path() / "ReadOnly";
	write_file(source, "stay");
	ops->fail_create_directories = true;
	const std::chrono::milliseconds budget = 200ms;
	FileMover mover(fast_options(ops, budget));

	const auto started = Clock::now();
	mover.move(source, dest_dir, log.callback());
	const auto entries = log.wait_for(1);
	const auto elapsed = Clock::now() - started;
	const MoveResult &r = entries[0].result;
	CHECK(r.status == MoveStatus::failed);
	CHECK(r.error.rfind("could not create folder", 0) == 0);
	CHECK(elapsed >= budget);
	CHECK(elapsed < budget + 5s);
	CHECK(read_file(source) == "stay");
	CHECK(!fs::exists(dest_dir));
}

TEST_CASE("wait_stable reports a file that grows and then stops as stable", "[file-mover]")
{
	TempDir tmp;
	StableLog log;
	FileMover mover(fast_options());
	const fs::path file = tmp.path() / "growing.mkv";
	write_file(file, "0");

	mover.wait_stable(file, 60ms, 10s, log.callback());
	// Keep writing for a while. The outcome does not depend on the timing here:
	// the file must settle, and the wait must then see it unchanged for 60 ms.
	std::string content = "0";
	for (int i = 0; i < 5; ++i) {
		content += "123";
		write_file(file, content);
		std::this_thread::sleep_for(15ms);
	}
	const auto entries = log.wait_for(1);
	CHECK(entries[0].result.status == StableStatus::stable);
	CHECK(entries[0].result.file == file);
	CHECK(entries[0].thread != std::this_thread::get_id());
}

TEST_CASE("wait_stable times out for a file that never appears", "[file-mover]")
{
	TempDir tmp;
	StableLog log;
	FileMover mover(fast_options());
	const std::chrono::milliseconds timeout = 150ms;

	const auto started = Clock::now();
	mover.wait_stable(tmp.path() / "never.mkv", 50ms, timeout, log.callback());
	const auto entries = log.wait_for(1);
	const auto elapsed = Clock::now() - started;
	CHECK(entries[0].result.status == StableStatus::timed_out);
	CHECK(elapsed >= timeout);
	CHECK(elapsed < timeout + 5s);
}

TEST_CASE("wait_stable keeps waiting for a file that appears later", "[file-mover]")
{
	TempDir tmp;
	StableLog log;
	FileMover mover(fast_options());
	const fs::path file = tmp.path() / "late.mkv";

	mover.wait_stable(file, 40ms, 10s, log.callback());
	// The worker is polling a missing file by now. Creating it must not end the wait early.
	std::this_thread::sleep_for(30ms);
	write_file(file, "arrived");
	const auto entries = log.wait_for(1);
	CHECK(entries[0].result.status == StableStatus::stable);
}

TEST_CASE("shutdown(false) cancels a running wait and every queued job, promptly", "[file-mover]")
{
	TempDir tmp;
	StableLog stables;
	MoveLog moves;
	// The watched file never appears, so the wait would run for a full minute.
	const fs::path watched = tmp.path() / "never.mkv";
	auto ops = std::make_shared<FakeOps>(watched);
	std::future<void> running = ops->started();
	FileMover mover(fast_options(ops));
	mover.wait_stable(watched, 50ms, 60s, stables.callback());
	REQUIRE(running.wait_for(kWaitLimit) == std::future_status::ready); // the wait is running now

	const fs::path first = tmp.path() / "first.mkv";
	const fs::path second = tmp.path() / "second.mkv";
	write_file(first, "1");
	write_file(second, "2");
	mover.move(first, tmp.path() / "Game", moves.callback());
	mover.move(second, tmp.path() / "Game", moves.callback());

	const auto before = Clock::now();
	mover.shutdown(false);
	CHECK(Clock::now() - before < 10s); // far below the 60 s timeout

	// shutdown returned, so every callback has fired exactly once.
	const auto stable_entries = stables.snapshot();
	REQUIRE(stable_entries.size() == 1);
	CHECK(stable_entries[0].result.status == StableStatus::cancelled);
	const auto move_entries = moves.snapshot();
	REQUIRE(move_entries.size() == 2);
	for (const auto &entry : move_entries)
		CHECK(entry.result.status == MoveStatus::cancelled);
	// Cancelled jobs touch nothing.
	CHECK(read_file(first) == "1");
	CHECK(read_file(second) == "2");
	CHECK(!fs::exists(tmp.path() / "Game"));
}

TEST_CASE("shutdown(false) ends a move that is still retrying", "[file-mover]")
{
	TempDir tmp;
	MoveLog log;
	const fs::path source = tmp.path() / "busy.mkv";
	write_file(source, "busy");
	auto ops = std::make_shared<FakeOps>(source);
	std::future<void> running = ops->started();
	ops->rename_fail_from = source;
	ops->rename_fail_error = std::make_error_code(std::errc::permission_denied);
	ops->rename_fail_times = -1;
	// Longer than any test runs, and as large as a budget can be: only shutdown can end the retries,
	// and the budget's deadline must not overflow the clock.
	FileMover mover(fast_options(ops, std::chrono::milliseconds::max()));

	mover.move(source, tmp.path() / "Game", log.callback());
	REQUIRE(running.wait_for(kWaitLimit) == std::future_status::ready); // the first attempt has begun
	const auto before = Clock::now();
	mover.shutdown(false);
	CHECK(Clock::now() - before < 10s);

	const auto entries = log.snapshot();
	REQUIRE(entries.size() == 1);
	CHECK(entries[0].result.status == MoveStatus::failed);
	CHECK(entries[0].result.error.rfind("shutting down", 0) == 0);
	CHECK(read_file(source) == "busy");
}

TEST_CASE("shutdown(true) runs every queued move, in order, before returning", "[file-mover]")
{
	TempDir tmp;
	MoveLog log;
	FileMover mover(fast_options());
	const fs::path dest_dir = tmp.path() / "Game";
	std::vector<fs::path> sources;
	for (int i = 0; i < 5; ++i) {
		const fs::path source = tmp.path() / path_from_utf8("clip-" + std::to_string(i) + ".mkv");
		write_file(source, std::to_string(i));
		sources.push_back(source);
		mover.move(source, dest_dir, log.callback());
	}
	mover.shutdown(true);

	// shutdown(true) returned, so all five callbacks have fired.
	const auto entries = log.snapshot();
	REQUIRE(entries.size() == 5);
	for (std::size_t i = 0; i < entries.size(); ++i) {
		const MoveResult &r = entries[i].result;
		CHECK(r.id == i + 1);
		CHECK(r.status == MoveStatus::moved);
		CHECK(read_file(r.final_path) == std::to_string(i));
		CHECK(!fs::exists(sources[i]));
	}
}

TEST_CASE("move and wait_stable after shutdown are cancelled on the calling thread", "[file-mover]")
{
	TempDir tmp;
	MoveLog moves;
	StableLog stables;
	FileMover mover(fast_options());
	mover.shutdown(false);
	const fs::path source = tmp.path() / "late.mkv";
	write_file(source, "late");

	const JobId move_id = mover.move(source, tmp.path() / "Game", moves.callback());
	const JobId stable_id = mover.wait_stable(source, 10ms, 10ms, stables.callback());

	// Both callbacks already ran, synchronously, on this thread.
	const auto move_entries = moves.snapshot();
	REQUIRE(move_entries.size() == 1);
	CHECK(move_entries[0].result.status == MoveStatus::cancelled);
	CHECK(move_entries[0].result.id == move_id);
	CHECK(move_entries[0].thread == std::this_thread::get_id());
	const auto stable_entries = stables.snapshot();
	REQUIRE(stable_entries.size() == 1);
	CHECK(stable_entries[0].result.status == StableStatus::cancelled);
	CHECK(stable_entries[0].result.id == stable_id);
	CHECK(stable_entries[0].thread == std::this_thread::get_id());
	CHECK(move_id < stable_id);
	CHECK(read_file(source) == "late");

	// Idempotent: repeated calls with either flag return normally.
	mover.shutdown(true);
	mover.shutdown(false);
}

TEST_CASE("a callback can queue more work, and a throwing callback does not stop the worker", "[file-mover]")
{
	TempDir tmp;
	MoveLog log;
	std::promise<MoveResult> chained;
	std::future<MoveResult> chained_done = chained.get_future();
	const fs::path dest_dir = tmp.path() / "Game";
	const fs::path first = tmp.path() / "first.mkv";
	const fs::path second = tmp.path() / "second.mkv";
	const fs::path third = tmp.path() / "third.mkv";
	write_file(first, "1");
	write_file(second, "2");
	write_file(third, "3");
	// Declared last, so the mover (and its worker) is destroyed before the state the callbacks use.
	FileMover mover(fast_options());

	mover.move(first, dest_dir, [&mover, &dest_dir, &second, &chained](const MoveResult &) {
		mover.move(second, dest_dir, [&chained](const MoveResult &r) { chained.set_value(r); });
		throw std::runtime_error("callback failure");
	});
	REQUIRE(chained_done.wait_for(kWaitLimit) == std::future_status::ready);
	CHECK(chained_done.get().status == MoveStatus::moved);

	// The worker survived the throw, so a later job still runs.
	mover.move(third, dest_dir, log.callback());
	const auto entries = log.wait_for(1);
	CHECK(entries[0].result.status == MoveStatus::moved);
	CHECK(read_file(dest_dir / "third.mkv") == "3");
}

TEST_CASE("huge timeouts and budgets are clamped instead of overflowing the clock", "[file-mover]")
{
	TempDir tmp;
	StableLog log;
	FileMover mover(fast_options(nullptr, std::chrono::milliseconds::max()));
	const fs::path file = tmp.path() / "huge.mkv";

	mover.wait_stable(file, 40ms, std::chrono::milliseconds::max(), log.callback());
	std::this_thread::sleep_for(30ms);
	write_file(file, "appeared");
	const auto entries = log.wait_for(1);
	CHECK(entries[0].result.status == StableStatus::stable);

	// A stable window as large as it can be: the file is not that old, so the wait times out.
	const fs::path steady = tmp.path() / "steady.mkv";
	write_file(steady, "fixed");
	mover.wait_stable(steady, std::chrono::milliseconds::max(), 200ms, log.callback());
	const auto all = log.wait_for(2);
	CHECK(all[1].result.status == StableStatus::timed_out);
}

TEST_CASE("default_file_ops never replaces a file and reports missing ones quietly", "[file-mover]")
{
	TempDir tmp;
	auto ops = FileMover::default_file_ops();
	const fs::path a = tmp.path() / "a.txt";
	const fs::path b = tmp.path() / "b.txt";
	const fs::path c = tmp.path() / "c.txt";
	const fs::path missing = tmp.path() / "missing.txt";
	write_file(a, "from-a");
	write_file(b, "from-b");

	// rename and copy both refuse to overwrite b.
	CHECK(ops->rename(a, b) == std::make_error_code(std::errc::file_exists));
	const std::error_code copy_error = ops->copy_file(a, b);
	CHECK(static_cast<bool>(copy_error));
	CHECK(read_file(a) == "from-a");
	CHECK(read_file(b) == "from-b");

	// Missing files: no throw, no size, no time.
	CHECK(!ops->exists(missing));
	CHECK(!ops->file_size(missing).has_value());
	CHECK(!ops->last_write_time(missing).has_value());

	// A rename to a free name works, and the size is reported.
	CHECK(!ops->rename(a, c));
	CHECK(read_file(c) == "from-a");
	CHECK(!fs::exists(a));
	CHECK(ops->file_size(c).value_or(0) == 6);
}

TEST_CASE("the default constructor moves files with the real filesystem", "[file-mover]")
{
	TempDir tmp;
	MoveLog log;
	FileMover mover;
	const fs::path source = tmp.path() / "default.mkv";
	write_file(source, "real");

	mover.move(source, tmp.path() / "Game", log.callback());
	const auto entries = log.wait_for(1);
	CHECK(entries[0].result.status == MoveStatus::moved);
	CHECK(read_file(entries[0].result.final_path) == "real");
	CHECK(!fs::exists(source));
}

TEST_CASE("wait_stable never reports before the full window has passed since the last change", "[file-mover]")
{
	TempDir tmp;
	StableLog log;
	FileMover mover(fast_options());
	const fs::path file = tmp.path() / "settle.mkv";
	const std::chrono::milliseconds window = 300ms;
	// The last change is the write below, which starts after this time point. Starting the
	// clock early can only make the bound lenient.
	const Clock::time_point last_change = Clock::now();
	write_file(file, "settled");

	mover.wait_stable(file, window, 10s, log.callback());
	const auto entries = log.wait_for(1);
	REQUIRE(entries[0].result.status == StableStatus::stable);
	// Only the early bound is tight. A window cut in half reports about 150 ms after the
	// change and fails here. The 25 ms allowance only covers timer noise.
	CHECK(entries[0].when - last_change >= window - 25ms);
}

TEST_CASE("a change of modification time alone restarts the stability window", "[file-mover]")
{
	TempDir tmp;
	StableLog log;
	FileMover mover(fast_options());
	const fs::path file = tmp.path() / "same-size.mkv";
	const std::chrono::milliseconds window = 300ms;
	write_file(file, "same-size");

	mover.wait_stable(file, window, 10s, log.callback());
	// Part of the window is used up, then the file is only touched. Its size stays the same.
	std::this_thread::sleep_for(100ms);
	const Clock::time_point touched = Clock::now();
	fs::last_write_time(file, fs::last_write_time(file) + std::chrono::hours(1));

	const auto entries = log.wait_for(1);
	REQUIRE(entries[0].result.status == StableStatus::stable);
	// Ignoring mtime would report about 200 ms after the touch, which fails this bound.
	CHECK(entries[0].when - touched >= window - 25ms);
}

TEST_CASE("a zero retry budget gives exactly one attempt", "[file-mover]")
{
	TempDir tmp;
	MoveLog log;
	auto ops = std::make_shared<FakeOps>();
	const fs::path source = tmp.path() / "once.mkv";
	write_file(source, "once");
	ops->rename_fail_from = source;
	ops->rename_fail_error = std::make_error_code(std::errc::permission_denied);
	ops->rename_fail_times = -1;
	FileMover mover(fast_options(ops, 0ms));

	mover.move(source, tmp.path() / "Game", log.callback());
	const auto entries = log.wait_for(1);
	CHECK(entries[0].result.status == MoveStatus::failed);
	CHECK(ops->rename_calls.load() == 1);
	CHECK(read_file(source) == "once");
}

TEST_CASE("a source that vanishes during the copy leaves the partial as the only copy", "[file-mover]")
{
	TempDir tmp;
	MoveLog log;
	auto ops = std::make_shared<FakeOps>();
	const fs::path source = tmp.path() / "only.mkv";
	const fs::path dest_dir = tmp.path() / "Game";
	write_file(source, "ONLY-COPY-BYTES");
	ops->rename_fail_from = source;
	ops->rename_fail_error = std::make_error_code(std::errc::cross_device_link);
	ops->rename_fail_times = -1;
	// Another process deletes the source right after the copy finishes.
	ops->after_copy = [](const fs::path &from, const fs::path &) {
		std::error_code ec;
		fs::remove(from, ec);
	};
	FileMover mover(fast_options(ops));

	mover.move(source, dest_dir, log.callback());
	const auto entries = log.wait_for(1);
	const MoveResult &r = entries[0].result;
	CHECK(r.status == MoveStatus::failed);
	CHECK(r.final_path == dest_dir / "only.mkv.clip-atlas-partial");
	CHECK(r.error == "source disappeared during the copy; partial copy kept at only.mkv.clip-atlas-partial");
	CHECK(read_file(r.final_path) == "ONLY-COPY-BYTES");
	CHECK(!fs::exists(dest_dir / "only.mkv"));
}

TEST_CASE("a writer that appends after verification keeps the source and the published copy", "[file-mover]")
{
	TempDir tmp;
	MoveLog log;
	auto ops = std::make_shared<FakeOps>();
	const fs::path source = tmp.path() / "writer.mkv";
	const fs::path dest_dir = tmp.path() / "Game";
	const fs::path partial = dest_dir / "writer.mkv.clip-atlas-partial";
	write_file(source, "ORIGINAL");
	ops->rename_fail_from = source;
	ops->rename_fail_error = std::make_error_code(std::errc::cross_device_link);
	ops->rename_fail_times = -1;
	// The append lands after the size check and before the pre-delete check. Hooking the
	// publish rename makes the writer deterministic. An append inside remove() itself
	// cannot be seen by any check made before the delete.
	ops->before_rename = [&source, &partial](const fs::path &from, const fs::path &) {
		if (from == partial) {
			std::ofstream out(source, std::ios::binary | std::ios::app);
			out << "+LATE-WRITE";
		}
	};
	FileMover mover(fast_options(ops));

	mover.move(source, dest_dir, log.callback());
	const auto entries = log.wait_for(1);
	const MoveResult &r = entries[0].result;
	CHECK(r.status == MoveStatus::copied_source_kept);
	CHECK(r.error == "source changed during the move");
	CHECK(r.final_path == dest_dir / "writer.mkv");
	CHECK(read_file(source) == "ORIGINAL+LATE-WRITE");
	CHECK(read_file(r.final_path) == "ORIGINAL");
}

TEST_CASE("a source that changes while it is copied is copied again", "[file-mover]")
{
	TempDir tmp;
	MoveLog log;
	auto ops = std::make_shared<FakeOps>();
	const fs::path source = tmp.path() / "growing.mkv";
	const fs::path dest_dir = tmp.path() / "Game";
	write_file(source, "first");
	ops->rename_fail_from = source;
	ops->rename_fail_error = std::make_error_code(std::errc::cross_device_link);
	ops->rename_fail_times = -1;
	std::atomic<bool> appended{false};
	// The first copy is torn: a writer appends right after it. Later copies see a settled file.
	ops->after_copy = [&appended, &source](const fs::path &, const fs::path &) {
		if (!appended.exchange(true)) {
			std::ofstream out(source, std::ios::binary | std::ios::app);
			out << "+more";
		}
	};
	FileMover mover(fast_options(ops));

	mover.move(source, dest_dir, log.callback());
	const auto entries = log.wait_for(1);
	const MoveResult &r = entries[0].result;
	CHECK(r.status == MoveStatus::moved);
	CHECK(r.final_path == dest_dir / "growing.mkv");
	CHECK(read_file(r.final_path) == "first+more");
	CHECK(!fs::exists(source));
	CHECK(!fs::exists(dest_dir / "growing.mkv.clip-atlas-partial"));
}

TEST_CASE("a copy that fails after writing part of the partial removes it and a later attempt succeeds", "[file-mover]")
{
	TempDir tmp;
	MoveLog log;
	auto ops = std::make_shared<FakeOps>();
	const fs::path source = tmp.path() / "retry.mkv";
	const fs::path dest_dir = tmp.path() / "Game";
	write_file(source, "payload");
	ops->rename_fail_from = source;
	ops->rename_fail_error = std::make_error_code(std::errc::cross_device_link);
	ops->rename_fail_times = -1;
	ops->copy_fail_times = 1; // the first copy writes "junk" into the partial, then fails
	FileMover mover(fast_options(ops));

	mover.move(source, dest_dir, log.callback());
	const auto entries = log.wait_for(1);
	const MoveResult &r = entries[0].result;
	CHECK(r.status == MoveStatus::moved);
	// A leftover partial would have pushed the result to "retry (2).mkv".
	CHECK(r.final_path == dest_dir / "retry.mkv");
	CHECK(read_file(r.final_path) == "payload");
	CHECK(!fs::exists(dest_dir / "retry.mkv.clip-atlas-partial"));
	CHECK(!fs::exists(source));
}

TEST_CASE("a failed publish removes the partial and leaves the source in place", "[file-mover]")
{
	TempDir tmp;
	MoveLog log;
	auto ops = std::make_shared<FakeOps>();
	const fs::path source = tmp.path() / "publish.mkv";
	const fs::path dest_dir = tmp.path() / "Game";
	const fs::path partial = dest_dir / "publish.mkv.clip-atlas-partial";
	write_file(source, "keep");
	ops->rename_fail_from = source;
	ops->rename_fail_error = std::make_error_code(std::errc::cross_device_link);
	ops->rename_fail_times = -1;
	// Only the publish of the partial fails. The copy itself is fine.
	ops->rename_override = [partial](const fs::path &from, const fs::path &) {
		return from == partial ? std::make_error_code(std::errc::io_error) : std::error_code();
	};
	FileMover mover(fast_options(ops, 200ms));

	mover.move(source, dest_dir, log.callback());
	const auto entries = log.wait_for(1);
	CHECK(entries[0].result.status == MoveStatus::failed);
	CHECK(read_file(source) == "keep");
	CHECK(!fs::exists(partial));
	CHECK(!fs::exists(dest_dir / "publish.mkv"));
}

TEST_CASE("an exception while publishing leaves no partial and still reports once", "[file-mover]")
{
	TempDir tmp;
	MoveLog log;
	auto ops = std::make_shared<FakeOps>();
	const fs::path source = tmp.path() / "throws.mkv";
	const fs::path dest_dir = tmp.path() / "Game";
	write_file(source, "payload");
	ops->rename_fail_from = source;
	ops->rename_fail_error = std::make_error_code(std::errc::cross_device_link);
	ops->rename_fail_times = -1;
	ops->throw_on_rename_from = dest_dir / "throws.mkv.clip-atlas-partial";
	FileMover mover(fast_options(ops, 200ms));

	mover.move(source, dest_dir, log.callback());
	const auto entries = log.wait_for(1);
	CHECK(entries[0].result.status == MoveStatus::failed);
	CHECK(!fs::exists(dest_dir / "throws.mkv.clip-atlas-partial"));
	CHECK(read_file(source) == "payload");
	mover.shutdown(true);
	CHECK(log.snapshot().size() == 1);
}

TEST_CASE("a slow copy does not use up the time the source removal needs", "[file-mover]")
{
	TempDir tmp;
	MoveLog log;
	auto ops = std::make_shared<FakeOps>();
	const fs::path source = tmp.path() / "slow.mkv";
	const fs::path dest_dir = tmp.path() / "Game";
	write_file(source, "slow");
	ops->rename_fail_from = source;
	ops->rename_fail_error = std::make_error_code(std::errc::cross_device_link);
	ops->rename_fail_times = -1;
	ops->copy_delay = 100ms;        // the copy alone outlasts the 50 ms budget
	ops->remove_busy_path = source; // the first delete of the source is refused
	ops->remove_busy_times = 1;
	FileMover mover(fast_options(ops, 50ms));

	mover.move(source, dest_dir, log.callback());
	const auto entries = log.wait_for(1);
	const MoveResult &r = entries[0].result;
	CHECK(r.status == MoveStatus::moved);
	CHECK(read_file(r.final_path) == "slow");
	CHECK(!fs::exists(source));
}

TEST_CASE("a partial that is refused twice is still cleaned up after a failed copy", "[file-mover]")
{
	TempDir tmp;
	MoveLog log;
	auto ops = std::make_shared<FakeOps>();
	const fs::path source = tmp.path() / "locked.mkv";
	const fs::path dest_dir = tmp.path() / "Game";
	const fs::path partial = dest_dir / "locked.mkv.clip-atlas-partial";
	write_file(source, "payload");
	ops->rename_fail_from = source;
	ops->rename_fail_error = std::make_error_code(std::errc::cross_device_link);
	ops->rename_fail_times = -1;
	ops->copy_delay = 100ms;  // the copy alone outlasts the 50 ms budget
	ops->copy_fail_times = 1; // writes "junk" into the partial, then fails
	ops->remove_busy_path = partial;
	ops->remove_busy_times = 2; // the cleanup is refused twice before it works
	FileMover mover(fast_options(ops, 50ms));

	mover.move(source, dest_dir, log.callback());
	const auto entries = log.wait_for(1);
	CHECK(entries[0].result.status == MoveStatus::failed);
	CHECK(!fs::exists(partial)); // no orphan
	CHECK(read_file(source) == "payload");
	CHECK(!fs::exists(dest_dir / "locked.mkv"));
}

TEST_CASE("shutdown(false) after shutdown(true) aborts the drain promptly", "[file-mover]")
{
	TempDir tmp;
	StableLog log;
	FileMover mover(fast_options());
	// The watched file never appears, so the wait would run for a minute if nothing stopped it.
	mover.wait_stable(tmp.path() / "never.mkv", 50ms, 60s, log.callback());
	std::thread drainer([&mover] { mover.shutdown(true); });
	std::this_thread::sleep_for(100ms); // lets the draining shutdown start; the outcome does not depend on it
	const auto before = Clock::now();
	mover.shutdown(false);
	const auto took = Clock::now() - before;
	drainer.join();
	CHECK(took < 5s);
	const auto entries = log.snapshot();
	REQUIRE(entries.size() == 1);
	CHECK(entries[0].result.status == StableStatus::cancelled);
}

TEST_CASE("shutdown called from inside a callback returns instead of deadlocking", "[file-mover]")
{
	TempDir tmp;
	std::promise<void> done;
	std::future<void> finished = done.get_future();
	const fs::path source = tmp.path() / "self.mkv";
	write_file(source, "self");
	FileMover mover(fast_options());
	mover.move(source, tmp.path() / "Game", [&mover, &done](const MoveResult &) {
		mover.shutdown(false);
		done.set_value();
	});
	REQUIRE(finished.wait_for(kWaitLimit) == std::future_status::ready);
}

TEST_CASE("racing producers and shutdown calls report every job exactly once", "[file-mover]")
{
	TempDir tmp;
	constexpr int kProducers = 4;
	constexpr int kPerProducer = 100;
	std::vector<std::atomic<int>> calls(kProducers * kPerProducer);
	{
		FileMover mover(fast_options(nullptr, 50ms));
		std::vector<std::thread> producers;
		for (int p = 0; p < kProducers; ++p) {
			producers.emplace_back([&, p] {
				for (int i = 0; i < kPerProducer; ++i) {
					const int idx = p * kPerProducer + i;
					if (i % 3 == 0) {
						mover.wait_stable(tmp.path() / "absent.mkv", 1ms, 2ms,
								  [&calls, idx](const StableResult &) {
									  ++calls[idx];
								  });
					} else {
						const fs::path source =
							tmp.path() / path_from_utf8("f" + std::to_string(idx) + ".mkv");
						mover.move(source, tmp.path() / "Game",
							   [&calls, idx](const MoveResult &) { ++calls[idx]; });
					}
				}
			});
		}
		std::thread first([&mover] {
			std::this_thread::sleep_for(2ms);
			mover.shutdown(false);
		});
		std::thread second([&mover] {
			std::this_thread::sleep_for(3ms);
			mover.shutdown(true);
		});
		for (std::thread &t : producers)
			t.join();
		first.join();
		second.join();
	}
	int bad = 0;
	for (const std::atomic<int> &c : calls) {
		if (c.load() != 1)
			++bad;
	}
	CHECK(bad == 0);
}

TEST_CASE("a directory is not a file: refused at once and left alone", "[file-mover]")
{
	TempDir tmp;
	MoveLog log;
	auto ops = std::make_shared<FakeOps>();
	const fs::path folder = tmp.path() / "Not A Clip";
	fs::create_directories(folder);
	write_file(folder / "inside.txt", "keep");
	FileMover mover(fast_options(ops, 5000ms));

	const auto started = Clock::now();
	mover.move(folder, tmp.path() / "Game", log.callback());
	const auto entries = log.wait_for(1);
	CHECK(Clock::now() - started < 2s);
	CHECK(entries[0].result.status == MoveStatus::failed);
	CHECK(entries[0].result.error == "source is not a file");
	CHECK(ops->rename_calls.load() == 0);
	CHECK(read_file(folder / "inside.txt") == "keep");
}

TEST_CASE("a clip already in the destination folder stays where it is", "[file-mover]")
{
	TempDir tmp;
	MoveLog log;
	FileMover mover(fast_options());
	const fs::path dest_dir = tmp.path() / "Game";
	fs::create_directories(dest_dir);
	const fs::path source = dest_dir / "here.mkv";
	write_file(source, "in place");

	mover.move(source, dest_dir, log.callback());
	const auto entries = log.wait_for(1);
	const MoveResult &r = entries[0].result;
	CHECK(r.status == MoveStatus::moved);
	CHECK(r.final_path == source);
	CHECK(read_file(source) == "in place");
	CHECK(!fs::exists(dest_dir / "here (2).mkv"));
}

TEST_CASE("a dangling symlink at the target name is never replaced", "[file-mover]")
{
	TempDir tmp;
	MoveLog log;
	FileMover mover(fast_options());
	const fs::path dest_dir = tmp.path() / "Game";
	fs::create_directories(dest_dir);
	std::error_code ec;
	fs::create_symlink(tmp.path() / "nowhere.mkv", dest_dir / "clip.mkv", ec);
	if (ec) {
		SKIP("symbolic links are not available on this system");
		return;
	}
	const fs::path source = tmp.path() / "clip.mkv";
	write_file(source, "NEW");

	mover.move(source, dest_dir, log.callback());
	const auto entries = log.wait_for(1);
	const MoveResult &r = entries[0].result;
	CHECK(r.status == MoveStatus::moved);
	CHECK(r.final_path != dest_dir / "clip.mkv");
	CHECK(fs::is_symlink(dest_dir / "clip.mkv"));
	CHECK(read_file(r.final_path) == "NEW");
}

TEST_CASE("UTF-8 file names survive a move", "[file-mover]")
{
	TempDir tmp;
	MoveLog log;
	FileMover mover(fast_options());
	const fs::path name = path_from_utf8("ポケモン 2026.mkv");
	const fs::path dest_dir = tmp.path() / path_from_utf8("ゲーム");
	const fs::path source = tmp.path() / name;
	write_file(source, "utf8");

	mover.move(source, dest_dir, log.callback());
	const auto entries = log.wait_for(1);
	const MoveResult &r = entries[0].result;
	CHECK(r.status == MoveStatus::moved);
	CHECK(r.final_path == dest_dir / name);
	CHECK(read_file(r.final_path) == "utf8");
	CHECK(!fs::exists(source));
}
