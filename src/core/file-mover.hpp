// Clip Atlas — moves clips on a background thread without ever losing one.
//
// One worker thread runs jobs in the order they were queued. Two job kinds:
//   - move: put a file into a folder under a free name (clip-layout's
//     unique_destination), by rename when possible and by verified copy +
//     delete across volumes.
//   - wait_stable: wait until a file exists and its size and modification
//     time have not changed for a while (e.g. OBS finishing a remux).
//
// The invariant: the source is deleted only after a copy of the same size
// exists at the destination. Every failure leaves the clip where it was.
//
// Callbacks run on the worker thread. The plugin hops to the UI thread from
// there. A callback may queue more jobs.
//
// Pure C++17 with no OBS dependency. Paths are std::filesystem::path; build
// them from UTF-8 with path_from_utf8().
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>

namespace clip_atlas {

// Filesystem operations the mover needs, so tests can inject failures.
// The default implementation (FileMover::default_file_ops()) uses
// std::filesystem with std::error_code overloads and never throws. Its
// rename() checks exists(to) first because std::filesystem::rename replaces;
// the gap between check and rename is accepted since only this worker writes
// into clip folders.
class FileOps {
public:
	virtual ~FileOps() = default;
	virtual bool exists(const std::filesystem::path &p) = 0;
	virtual std::error_code create_directories(const std::filesystem::path &dir) = 0;
	// Must not replace an existing `to`; return an error instead.
	virtual std::error_code rename(const std::filesystem::path &from, const std::filesystem::path &to) = 0;
	// Must not replace an existing `to`; return an error instead.
	virtual std::error_code copy_file(const std::filesystem::path &from, const std::filesystem::path &to) = 0;
	virtual std::error_code remove(const std::filesystem::path &p) = 0;
	virtual std::optional<std::uintmax_t> file_size(const std::filesystem::path &p) = 0;
	virtual std::optional<std::filesystem::file_time_type> last_write_time(const std::filesystem::path &p) = 0;
};

using JobId = std::uint64_t;

enum class MoveStatus {
	moved,              // source is now at final_path
	copied_source_kept, // verified copy at final_path, but the source could not be deleted
	failed,             // nothing changed; the clip is still at its source path
	cancelled,          // shut down before the job ran; nothing changed
};

struct MoveResult {
	JobId id = 0;
	MoveStatus status = MoveStatus::failed;
	std::filesystem::path source;
	std::filesystem::path final_path; // set for moved and copied_source_kept
	std::string error;                // human-readable reason, English, for the log
};

enum class StableStatus {
	stable,    // the file exists and was unchanged for the requested time
	timed_out, // never became stable (or never appeared) before the timeout
	cancelled, // shut down while waiting
};

struct StableResult {
	JobId id = 0;
	StableStatus status = StableStatus::timed_out;
	std::filesystem::path file;
};

class FileMover {
public:
	using MoveCallback = std::function<void(const MoveResult &)>;
	using StableCallback = std::function<void(const StableResult &)>;

	struct Options {
		// Total time to keep retrying a move whose source or destination is
		// locked (another process has the file open).
		std::chrono::milliseconds retry_budget{30000};
		std::chrono::milliseconds initial_backoff{250}; // doubles each retry
		std::chrono::milliseconds max_backoff{4000};
		// How often wait_stable looks at the file.
		std::chrono::milliseconds stable_poll_interval{500};
		// nullptr: default_file_ops().
		std::shared_ptr<FileOps> file_ops;
	};

	FileMover();
	explicit FileMover(Options options);
	// Calls shutdown(false).
	~FileMover();

	FileMover(const FileMover &) = delete;
	FileMover &operator=(const FileMover &) = delete;

	// Queues a move of `source` into `dest_dir` (created if missing). The final
	// name is unique_destination(dest_dir, source.filename()).
	//   - Missing source: failed at once, no retry.
	//   - rename() first. If it fails with std::errc::cross_device_link, fall
	//     back to copy: copy to "<final name>.clip-atlas-partial" in dest_dir,
	//     compare file_size() of copy and source, rename the partial to the
	//     final name, then remove the source. A size mismatch removes the
	//     partial and fails. If removing the source keeps failing until the
	//     retry budget runs out, the result is copied_source_kept.
	//   - Any other rename/copy error is retried with backoff until the retry
	//     budget is spent, then failed. Partial files are always cleaned up.
	// After shutdown() the job is not queued and the callback gets cancelled
	// immediately on the calling thread. Returns the job id either way.
	JobId move(const std::filesystem::path &source, const std::filesystem::path &dest_dir, MoveCallback callback);

	// Queues a wait for `file` to exist with size and last_write_time unchanged
	// across polls spanning at least `stable_for`. Gives up after `timeout`
	// measured from when the job starts running. Holds the worker while it
	// waits, so jobs queued after it run after it.
	JobId wait_stable(const std::filesystem::path &file, std::chrono::milliseconds stable_for,
			  std::chrono::milliseconds timeout, StableCallback callback);

	// Stops the worker and joins it. Idempotent; safe to call from any thread
	// except a callback.
	//   - wait_for_pending = true: queued jobs run to completion first.
	//   - wait_for_pending = false: a running copy finishes its current step,
	//     retries and stable waits stop at once with failed/cancelled, and
	//     queued jobs get cancelled.
	// Every queued job's callback has been called exactly once when this
	// returns.
	void shutdown(bool wait_for_pending);

	static std::shared_ptr<FileOps> default_file_ops();

private:
	struct Job;

	void run();

	Options options_;
	std::mutex mutex_;
	std::condition_variable wake_;
	std::deque<std::unique_ptr<Job>> queue_;
	bool stopping_ = false;
	bool drain_ = false;
	JobId next_id_ = 1;
	std::thread worker_;
};

} // namespace clip_atlas
