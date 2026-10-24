#include "file-mover.hpp"

#include "clip-layout.hpp"

#include <algorithm>
#include <utility>

namespace clip_atlas {

namespace fs = std::filesystem;

namespace {

using Clock = std::chrono::steady_clock;

// Longest wait the mover honours. It keeps `clock reading + duration` clear of the
// clock's overflow limit, even for a caller that passes milliseconds::max().
constexpr std::chrono::milliseconds kLongestWait = std::chrono::hours(24);

// Budget of each cleanup or source-removal step, counted from when that step starts.
// A slow copy must not use up the time these steps need to finish their own work.
constexpr std::chrono::milliseconds kStepBudget = std::chrono::seconds(5);

// Name suffix of the temporary copy made across volumes. A stray one left by a crash
// is recognisable by this suffix.
constexpr const char *kPartialSuffix = ".clip-atlas-partial";

// Set while a mover's worker runs. shutdown() checks it so a callback that calls
// shutdown() does not try to join the thread it is running on.
thread_local const FileMover *t_worker_owner = nullptr;

fs::path partial_for(const fs::path &final_path)
{
	fs::path partial = final_path.filename();
	partial += kPartialSuffix;
	return final_path.parent_path() / partial;
}

// The final component of `p`, as UTF-8 for messages. u8string() is std::string in
// C++17 and std::u8string in C++20, so the bytes are copied one by one.
std::string utf8_name(const fs::path &p)
{
	const auto name = p.filename().u8string();
	return std::string(name.begin(), name.end());
}

// Whether anything is present at `p`, of any type, including a dangling symlink.
// Any error other than "not found" also counts as present: the mover must not
// replace or delete something it cannot inspect.
bool entry_present(const fs::path &p)
{
	std::error_code ec;
	const fs::file_status st = fs::symlink_status(p, ec);
	if (ec)
		return ec != std::errc::no_such_file_or_directory;
	return st.type() != fs::file_type::not_found;
}

// The real filesystem. Every call uses the std::error_code overloads and catches
// anything that can still throw (bad_alloc, odd path input), so a filesystem
// surprise becomes an error the job reports instead of killing the worker.
class DefaultFileOps final : public FileOps {
public:
	bool exists(const fs::path &p) override
	{
		try {
			return entry_present(p);
		} catch (...) {
			return true;
		}
	}

	std::error_code create_directories(const fs::path &dir) override
	{
		try {
			std::error_code ec;
			fs::create_directories(dir, ec);
			return ec;
		} catch (...) {
			return std::make_error_code(std::errc::io_error);
		}
	}

	std::error_code rename(const fs::path &from, const fs::path &to) override
	{
		try {
			// std::filesystem::rename replaces `to`, and rename(2) replaces a dangling
			// symlink too, so any entry at `to` is refused.
			if (entry_present(to))
				return std::make_error_code(std::errc::file_exists);
			std::error_code ec;
			fs::rename(from, to, ec);
			return ec;
		} catch (...) {
			return std::make_error_code(std::errc::io_error);
		}
	}

	std::error_code copy_file(const fs::path &from, const fs::path &to) override
	{
		try {
			std::error_code ec;
			// copy_options::none makes the copy fail if `to` exists, so an
			// existing file is never overwritten.
			fs::copy_file(from, to, fs::copy_options::none, ec);
			return ec;
		} catch (...) {
			return std::make_error_code(std::errc::io_error);
		}
	}

	std::error_code remove(const fs::path &p) override
	{
		try {
			std::error_code ec;
			fs::remove(p, ec);
			return ec;
		} catch (...) {
			return std::make_error_code(std::errc::io_error);
		}
	}

	std::optional<std::uintmax_t> file_size(const fs::path &p) override
	{
		try {
			std::error_code ec;
			const std::uintmax_t size = fs::file_size(p, ec);
			if (ec)
				return std::nullopt;
			return size;
		} catch (...) {
			return std::nullopt;
		}
	}

	std::optional<fs::file_time_type> last_write_time(const fs::path &p) override
	{
		try {
			std::error_code ec;
			const fs::file_time_type t = fs::last_write_time(p, ec);
			if (ec)
				return std::nullopt;
			return t;
		} catch (...) {
			return std::nullopt;
		}
	}
};

// Whether shutdown(false) has been requested, plus sleeps that it interrupts.
// The references point at FileMover's members and are read under its mutex.
// shutdown(true) is a normal drain, so it does not count as an abort.
class Abort {
public:
	Abort(std::mutex &mutex, std::condition_variable &wake, const bool &stopping, const bool &drain)
		: mutex_(mutex),
		  wake_(wake),
		  stopping_(stopping),
		  drain_(drain)
	{
	}

	bool requested() const
	{
		std::lock_guard<std::mutex> lock(mutex_);
		return stopping_ && !drain_;
	}

	// Sleeps until `until` or until an abort is requested. Returns requested().
	bool sleep_until(Clock::time_point until) const
	{
		std::unique_lock<std::mutex> lock(mutex_);
		wake_.wait_until(lock, until, [this] { return stopping_ && !drain_; });
		return stopping_ && !drain_;
	}

private:
	std::mutex &mutex_;
	std::condition_variable &wake_;
	const bool &stopping_;
	const bool &drain_;
};

// Exponential retry pacing: starts at initial_backoff, doubles up to max_backoff,
// and never runs past `budget`, which is counted from `started`. Sleeps are
// interruptible, so shutdown(false) ends them at once.
class Backoff {
public:
	Backoff(std::chrono::milliseconds budget, const FileMover::Options &options, Clock::time_point started)
		: deadline_(started + std::chrono::duration_cast<Clock::duration>(
					      std::clamp(budget, std::chrono::milliseconds::zero(), kLongestWait))),
		  step_(std::clamp(options.initial_backoff, std::chrono::milliseconds(1), kLongestWait)),
		  cap_(std::clamp(options.max_backoff, step_, kLongestWait))
	{
	}

	// Sleeps before the next attempt. False means give up: the budget is spent, or
	// shutdown(false) interrupted the wait.
	bool wait(const Abort &abort)
	{
		const Clock::time_point now = Clock::now();
		if (now >= deadline_ || abort.requested())
			return false;
		const Clock::time_point until =
			std::min(now + std::chrono::duration_cast<Clock::duration>(step_), deadline_);
		step_ = std::min(step_ * 2, cap_);
		return !abort.sleep_until(until);
	}

private:
	Clock::time_point deadline_;
	std::chrono::milliseconds step_;
	std::chrono::milliseconds cap_;
};

// The main attempt's schedule. Only lock and transient retries use it, and its
// budget runs from the start of the job.
Backoff job_backoff(const FileMover::Options &options)
{
	return Backoff(options.retry_budget, options, Clock::now());
}

// A cleanup or source-removal step has its own budget, at most kStepBudget, counted
// from when that step starts. A slow copy therefore cannot starve these steps.
Backoff step_backoff(const FileMover::Options &options)
{
	return Backoff(std::min(options.retry_budget, kStepBudget), options, Clock::now());
}

// What a writer changes: a file's size and modification time.
struct Stamp {
	std::uintmax_t size = 0;
	fs::file_time_type mtime{};

	bool operator==(const Stamp &other) const { return size == other.size && mtime == other.mtime; }
	bool operator!=(const Stamp &other) const { return !(*this == other); }
};

std::optional<Stamp> read_stamp(FileOps &ops, const fs::path &p)
{
	const std::optional<std::uintmax_t> size = ops.file_size(p);
	const std::optional<fs::file_time_type> mtime = ops.last_write_time(p);
	if (!size || !mtime)
		return std::nullopt;
	return Stamp{*size, *mtime};
}

// True once the file is gone: removed now, or absent already.
bool removed(const std::error_code &ec)
{
	return !ec || ec == std::errc::no_such_file_or_directory;
}

// Removes a partial copy this job made, retrying on `step`'s schedule. When
// shutdown(false) interrupts the wait, it makes one immediate final attempt without
// sleeping. Returns true when no partial is left.
bool remove_partial(FileOps &ops, const Abort &abort, Backoff &step, const fs::path &partial)
{
	std::error_code ec = ops.remove(partial);
	while (!removed(ec)) {
		if (!step.wait(abort)) {
			if (abort.requested())
				ec = ops.remove(partial);
			break;
		}
		ec = ops.remove(partial);
	}
	return removed(ec);
}

// Owns the partial copy of one cross-volume move from before copy_file until the
// partial is published. Unless disarmed, the destructor removes the partial, so an
// early return or an exception leaves nothing behind. The partial is never removed
// while the source is gone: then the partial is the only copy of the clip.
class PartialGuard {
public:
	PartialGuard(FileOps &ops, const FileMover::Options &options, const Abort &abort, const fs::path &source,
		     const fs::path &partial)
		: ops_(ops),
		  options_(options),
		  abort_(abort),
		  source_(source),
		  partial_(partial)
	{
	}

	PartialGuard(const PartialGuard &) = delete;
	PartialGuard &operator=(const PartialGuard &) = delete;

	~PartialGuard()
	{
		if (!armed_)
			return;
		try {
			cleanup();
		} catch (...) {
			// A destructor cannot throw. Leaving the partial on disk is the safe side.
		}
	}

	// Removes the partial now, once. Returns false when a partial is left behind.
	bool cleanup()
	{
		if (!armed_)
			return true;
		armed_ = false;
		if (!ops_.exists(source_))
			return false;
		Backoff step = step_backoff(options_);
		return remove_partial(ops_, abort_, step, partial_);
	}

	// Stops the guard from removing the partial: it was published, or it is the
	// only copy left.
	void disarm() { armed_ = false; }

private:
	FileOps &ops_;
	const FileMover::Options &options_;
	const Abort &abort_;
	fs::path source_;
	fs::path partial_;
	bool armed_ = true;
};

// The source vanished while the copy was running. The partial is then the only copy
// of the clip, so it is kept under its partial name and reported through final_path.
bool keep_only_copy(PartialGuard &guard, FileOps &ops, const fs::path &partial, MoveResult &result)
{
	guard.disarm();
	result.status = MoveStatus::failed;
	result.error = "source disappeared during the copy";
	if (ops.exists(partial)) {
		result.final_path = partial;
		result.error += "; partial copy kept at " + utf8_name(partial);
	} else {
		result.error += "; no partial copy exists";
	}
	return true;
}

// Checks whether the path is known to be something other than a regular file (a
// directory, for example). This one check bypasses FileOps, because it only reads
// and no injected FileOps needs a different answer. symlink_status is used, so a
// symlink is not a regular file either. An error is not proof, so it is not
// reported here: the rename decides those cases.
bool known_not_a_regular_file(const fs::path &p)
{
	std::error_code ec;
	const fs::file_status st = fs::symlink_status(p, ec);
	return !ec && st.type() != fs::file_type::not_found && !fs::is_regular_file(st);
}

// Whether the source already sits in the destination folder. This check reads the
// disk directly and writes nothing, so it bypasses FileOps. Without it,
// unique_destination would see the source's own name as taken and rename it "(2)".
bool same_folder(const fs::path &source, const fs::path &dest_dir)
{
	std::error_code ec;
	const bool same = fs::equivalent(source.parent_path(), dest_dir, ec);
	return !ec && same;
}

// Calls a user callback. A callback that throws must not kill the worker, and
// core has no logger (it cannot include OBS), so the exception is dropped. The
// plugin's callbacks catch and log their own errors.
template<typename Callback, typename Result> void deliver(const Callback &callback, const Result &result)
{
	if (!callback)
		return;
	try {
		callback(result);
	} catch (...) {
	}
}

// Cross-volume move. The order is the safety argument: copy under a partial name,
// verify, publish the copy, and only then delete the source. Returns true when
// `result` is final. Returns false for a transient failure with `problem` set; the
// caller retries the whole move.
bool copy_then_remove(FileOps &ops, const FileMover::Options &options, const Abort &abort, const fs::path &source,
		      const fs::path &dest_dir, MoveResult &result, std::string &problem)
{
	// The partial name must be free as well as the final one. A partial that already
	// exists belongs to someone else, so it counts as a name collision.
	const fs::path final_path = unique_destination(dest_dir, source.filename(), [&ops](const fs::path &p) {
		return ops.exists(p) || ops.exists(partial_for(p));
	});
	if (final_path.empty()) {
		result.status = MoveStatus::failed;
		result.error = "no free file name";
		return true;
	}
	const fs::path partial = partial_for(final_path);

	// Recorded before the copy, so a writer that changes the source afterwards is noticed.
	const std::optional<Stamp> before = read_stamp(ops, source);

	// Armed before copy_file: a copy that fails midway may already have written the partial.
	PartialGuard guard(ops, options, abort, source, partial);
	if (std::error_code ec = ops.copy_file(source, partial)) {
		if (ec == std::errc::file_exists) {
			// Created by someone else after our check, so it is not ours to remove.
			guard.disarm();
			problem = "copy target name was taken";
			return false;
		}
		if (!ops.exists(source))
			return keep_only_copy(guard, ops, partial, result);
		// A failed copy may have written part of the file, so the partial goes too.
		problem = "copy failed: " + ec.message();
		if (!guard.cleanup())
			problem += "; a partial copy was left in the destination folder";
		return false;
	}

	// The source is checked before anything else. A size read of a vanished source
	// would look like a mismatch, and the mismatch path would delete the only copy.
	if (!ops.exists(source))
		return keep_only_copy(guard, ops, partial, result);
	const std::optional<Stamp> after_copy = read_stamp(ops, source);
	if (!before || !after_copy) {
		problem = "could not read the source during the copy";
		if (!guard.cleanup())
			problem += "; a partial copy was left in the destination folder";
		return false;
	}
	if (*before != *after_copy) {
		// The source changed while it was copied, so the copy may be torn. Copy again.
		problem = "source changed during the copy";
		if (!guard.cleanup())
			problem += "; a partial copy was left in the destination folder";
		return false;
	}
	// The size check is the only evidence the copy is complete, so an unreadable
	// size counts as a mismatch.
	const std::optional<std::uintmax_t> copy_size = ops.file_size(partial);
	if (!copy_size || *copy_size != after_copy->size) {
		result.status = MoveStatus::failed;
		result.error = "copy size mismatch";
		if (!guard.cleanup())
			result.error += "; a partial copy was left in the destination folder";
		return true;
	}

	// Last check before publishing: the source must still be there.
	if (!ops.exists(source))
		return keep_only_copy(guard, ops, partial, result);
	if (std::error_code ec = ops.rename(partial, final_path)) {
		problem = "could not publish the copy: " + ec.message();
		if (!guard.cleanup())
			problem += "; a partial copy was left in the destination folder";
		return false;
	}
	// Published: the partial name is gone, so the guard has nothing left to undo.
	guard.disarm();
	result.final_path = final_path;

	// Immediately before the delete, the source must still be the file that was
	// verified. A source that is already gone has nothing left to delete.
	if (!ops.exists(source)) {
		result.status = MoveStatus::moved;
		return true;
	}
	if (read_stamp(ops, source) != before) {
		result.status = MoveStatus::copied_source_kept;
		result.error = "source changed during the move";
		return true;
	}

	// The verified copy is in place, so the source can go. The removal has its own budget.
	Backoff step = step_backoff(options);
	std::error_code removal = ops.remove(source);
	while (!removed(removal)) {
		if (!step.wait(abort)) {
			// Out of budget, or shutdown(false) asked us to stop waiting: one immediate final attempt.
			if (abort.requested())
				removal = ops.remove(source);
			break;
		}
		removal = ops.remove(source);
	}
	if (removed(removal)) {
		result.status = MoveStatus::moved;
		return true;
	}
	// The verified copy stays where it was published, and the source is kept too.
	result.status = MoveStatus::copied_source_kept;
	result.error = "source could not be removed: " + removal.message();
	if (abort.requested())
		result.error = "shutting down; " + result.error;
	return true;
}

// One pass over the steps of a move. Returns true when `result` is final, and false
// for a transient failure, with `problem` saying why. The caller decides whether to
// retry.
bool attempt_move(FileOps &ops, const FileMover::Options &options, const Abort &abort, const fs::path &source,
		  const fs::path &dest_dir, MoveResult &result, std::string &problem)
{
	const fs::path name = source.filename();
	if (name.empty()) {
		result.status = MoveStatus::failed;
		result.error = "source has no file name";
		return true;
	}
	// A missing source is final: waiting does not bring it back.
	if (!ops.exists(source)) {
		result.status = MoveStatus::failed;
		result.error = "source missing";
		return true;
	}
	if (known_not_a_regular_file(source)) {
		result.status = MoveStatus::failed;
		result.error = "source is not a file";
		return true;
	}
	if (same_folder(source, dest_dir)) {
		// Already in its destination folder: nothing to do on disk.
		result.status = MoveStatus::moved;
		result.final_path = source;
		return true;
	}
	if (std::error_code ec = ops.create_directories(dest_dir)) {
		problem = "could not create folder: " + ec.message();
		return false;
	}
	// Recomputed on every pass, because another file may have taken the name while we waited.
	const fs::path final_path =
		unique_destination(dest_dir, name, [&ops](const fs::path &p) { return ops.exists(p); });
	if (final_path.empty()) {
		result.status = MoveStatus::failed;
		result.error = "no free file name";
		return true;
	}
	const std::error_code ec = ops.rename(source, final_path);
	if (!ec) {
		result.status = MoveStatus::moved;
		result.final_path = final_path;
		return true;
	}
	// Rename cannot cross volumes, so the other volume is handled by a copy.
	if (ec == std::errc::cross_device_link)
		return copy_then_remove(ops, options, abort, source, dest_dir, result, problem);
	problem = "could not move the file: " + ec.message();
	return false;
}

// The whole move job. Transient failures are retried until the job's budget is spent
// or a shutdown(false) interrupts the wait. `result` belongs to the caller, so a copy
// that was already published survives an exception.
void perform_move(const FileMover::Options &options, FileOps &ops, const Abort &abort, MoveResult &result,
		  const fs::path &source, const fs::path &dest_dir)
{
	Backoff backoff = job_backoff(options);
	for (;;) {
		std::string problem;
		if (attempt_move(ops, options, abort, source, dest_dir, result, problem))
			return;
		if (backoff.wait(abort))
			continue;
		result.status = MoveStatus::failed;
		result.error = abort.requested() ? "shutting down: " + problem : problem;
		return;
	}
}

// The whole wait job. Stability needs at least `stable_for` of unchanged size and
// mtime, timed from the first reading of those values. A file that is missing or
// unreadable resets the window.
StableResult perform_wait_stable(const FileMover::Options &options, FileOps &ops, const Abort &abort, JobId id,
				 const fs::path &file, std::chrono::milliseconds stable_for,
				 std::chrono::milliseconds timeout)
{
	StableResult result;
	result.id = id;
	result.file = file;
	const Clock::time_point deadline =
		Clock::now() + std::chrono::duration_cast<Clock::duration>(
				       std::clamp(timeout, std::chrono::milliseconds::zero(), kLongestWait));
	// Clamped like the timeout: the comparison below converts this to nanoseconds.
	const Clock::duration stable_window = std::chrono::duration_cast<Clock::duration>(
		std::clamp(stable_for, std::chrono::milliseconds::zero(), kLongestWait));
	// A zero interval would spin the worker, so it is clamped to 1 ms.
	const std::chrono::milliseconds poll =
		std::clamp(options.stable_poll_interval, std::chrono::milliseconds(1), kLongestWait);

	bool observed = false; // true when `stamp` holds a reading
	Stamp stamp;
	Clock::time_point since; // when the current size and mtime were first seen

	for (;;) {
		if (abort.requested()) {
			result.status = StableStatus::cancelled;
			return result;
		}
		const Clock::time_point now = Clock::now();
		// The file may not exist yet. Keep waiting until it appears.
		const std::optional<Stamp> seen = ops.exists(file) ? read_stamp(ops, file) : std::nullopt;
		if (seen) {
			if (!observed || *seen != stamp) {
				stamp = *seen;
				since = now;
				observed = true;
			} else if (now - since >= stable_window) {
				result.status = StableStatus::stable;
				return result;
			}
		} else {
			observed = false;
		}
		if (now >= deadline) {
			result.status = StableStatus::timed_out;
			return result;
		}
		// Never sleep past the timeout.
		if (abort.sleep_until(std::min(now + std::chrono::duration_cast<Clock::duration>(poll), deadline))) {
			result.status = StableStatus::cancelled;
			return result;
		}
	}
}

} // namespace

// A queued request. `kind` picks which fields are used. The job owns its callback,
// so the callback outlives the caller's stack frame.
struct FileMover::Job {
	enum class Kind { move, wait_stable };

	Kind kind = Kind::move;
	JobId id = 0;

	fs::path source;
	fs::path dest_dir;
	MoveCallback move_cb;
	std::function<void()> on_started;

	fs::path file;
	std::chrono::milliseconds stable_for{0};
	std::chrono::milliseconds timeout{0};
	StableCallback stable_cb;
};

FileMover::FileMover() : FileMover(Options{}) {}

FileMover::FileMover(Options options) : options_(std::move(options))
{
	if (!options_.file_ops)
		options_.file_ops = default_file_ops();
	// Started last so the worker sees every member fully initialised.
	worker_ = std::thread([this] { run(); });
}

FileMover::~FileMover()
{
	shutdown(false);
}

JobId FileMover::move(const fs::path &source, const fs::path &dest_dir, MoveCallback callback,
		      std::function<void()> on_started)
{
	auto job = std::make_unique<Job>();
	job->kind = Job::Kind::move;
	job->source = source;
	job->dest_dir = dest_dir;
	job->move_cb = std::move(callback);
	job->on_started = std::move(on_started);

	JobId id = 0;
	bool accepted = false;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		id = next_id_++;
		job->id = id;
		if (!stopping_) {
			queue_.push_back(std::move(job));
			accepted = true;
		}
	}
	if (accepted) {
		wake_.notify_one();
		return id;
	}
	// After shutdown the job never runs. Report it here, on the caller's thread,
	// outside the lock, so a callback that queues more work cannot deadlock.
	MoveResult result;
	result.id = id;
	result.status = MoveStatus::cancelled;
	result.source = source;
	result.error = "shut down before the job ran";
	deliver(job->move_cb, result);
	return id;
}

JobId FileMover::wait_stable(const fs::path &file, std::chrono::milliseconds stable_for,
			     std::chrono::milliseconds timeout, StableCallback callback)
{
	auto job = std::make_unique<Job>();
	job->kind = Job::Kind::wait_stable;
	job->file = file;
	job->stable_for = stable_for;
	job->timeout = timeout;
	job->stable_cb = std::move(callback);

	JobId id = 0;
	bool accepted = false;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		id = next_id_++;
		job->id = id;
		if (!stopping_) {
			queue_.push_back(std::move(job));
			accepted = true;
		}
	}
	if (accepted) {
		wake_.notify_one();
		return id;
	}
	StableResult result;
	result.id = id;
	result.status = StableStatus::cancelled;
	result.file = file;
	deliver(job->stable_cb, result);
	return id;
}

void FileMover::shutdown(bool wait_for_pending)
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!stopping_) {
			stopping_ = true;
			drain_ = wait_for_pending;
		} else if (!wait_for_pending) {
			// A shutdown(false) after a draining shutdown(true) ends the drain.
			drain_ = false;
		}
	}
	wake_.notify_all();

	// A callback runs on the worker, which cannot join itself. The worker exits by
	// itself once the callback returns.
	if (t_worker_owner == this)
		return;
	// Two threads joining the same std::thread is a data race. Holding mutex_ across
	// join() would deadlock, because the worker needs mutex_ to finish.
	std::lock_guard<std::mutex> join_lock(join_mutex_);
	if (worker_.joinable())
		worker_.join();
}

void FileMover::run()
{
	t_worker_owner = this;
	FileOps &ops = *options_.file_ops;
	const Abort abort(mutex_, wake_, stopping_, drain_);
	for (;;) {
		std::unique_ptr<Job> job;
		std::deque<std::unique_ptr<Job>> cancelled;
		{
			std::unique_lock<std::mutex> lock(mutex_);
			wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
			if (stopping_ && !drain_) {
				// shutdown(false): nothing still queued starts. Take the jobs out
				// now and cancel them below, outside the lock.
				cancelled.swap(queue_);
			} else if (!queue_.empty()) {
				job = std::move(queue_.front());
				queue_.pop_front();
			} else {
				// shutdown(true) and nothing is left to run.
				return;
			}
		}

		if (!job) {
			for (const std::unique_ptr<Job> &pending : cancelled) {
				if (pending->kind == Job::Kind::move) {
					MoveResult result;
					result.id = pending->id;
					result.status = MoveStatus::cancelled;
					result.source = pending->source;
					result.error = "shut down before the job ran";
					deliver(pending->move_cb, result);
				} else {
					StableResult result;
					result.id = pending->id;
					result.status = StableStatus::cancelled;
					result.file = pending->file;
					deliver(pending->stable_cb, result);
				}
			}
			return;
		}

		// Every job gets exactly one callback. An exception from the filesystem layer
		// becomes a failed result, or copied_source_kept when the copy was already
		// published, instead of killing the worker.
		if (job->kind == Job::Kind::move) {
			try {
				if (job->on_started)
					job->on_started();
			} catch (...) {
				// Observation must never change file handling or suppress its result.
			}
			MoveResult result;
			result.id = job->id;
			result.source = job->source;
			try {
				perform_move(options_, ops, abort, result, job->source, job->dest_dir);
			} catch (...) {
				if (!result.final_path.empty()) {
					result.status = MoveStatus::copied_source_kept;
					result.error = "unexpected error after the copy was published";
				} else {
					result.status = MoveStatus::failed;
					result.error = "unexpected error";
				}
			}
			deliver(job->move_cb, result);
		} else {
			StableResult result;
			try {
				result = perform_wait_stable(options_, ops, abort, job->id, job->file, job->stable_for,
							     job->timeout);
			} catch (...) {
				result = StableResult{};
				result.id = job->id;
				result.file = job->file;
				result.status = StableStatus::timed_out;
			}
			deliver(job->stable_cb, result);
		}
	}
}

std::shared_ptr<FileOps> FileMover::default_file_ops()
{
	static const std::shared_ptr<FileOps> ops = std::make_shared<DefaultFileOps>();
	return ops;
}

} // namespace clip_atlas
