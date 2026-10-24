// Clip Atlas — a UI-independent projection of the filing lifecycle. UI thread only.
#pragma once

#include "clip-routing.hpp"

#include <chrono>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace clip_atlas {

enum class ActivityPhase { waiting, queued, moving, completed, failed, cancelled, unavailable };
enum class ActivityState { waiting, working, completed, failed, cancelled };
enum class ActivityNotice { none, stability_timeout, remux_unavailable, original_kept };

using ActivityClock = std::chrono::steady_clock;

struct ActivityFile {
	std::string path;
	bool remuxed = false;
	ActivityPhase phase = ActivityPhase::waiting;
	bool stability_confirmed = false;
	ActivityNotice notice = ActivityNotice::none;
	std::string detail;
	ActivityClock::time_point changed_at{};
};

// An accepted event declares all expected files up front, including remuxed outputs.
// Later events address a file by index, so repeated paths cannot finish a save twice.
struct ActivityEvent {
	std::uint64_t save_id = 0;
	bool accepted = false;
	ClipKind kind = ClipKind::replay;
	std::string game;
	std::vector<ActivityFile> files;
	std::size_t file_index = 0;
	ActivityPhase phase = ActivityPhase::waiting;
	bool stability_confirmed = false;
	ActivityNotice notice = ActivityNotice::none;
	std::string detail;
	ActivityClock::time_point at = ActivityClock::now();
};

struct Activity {
	std::uint64_t save_id = 0;
	ClipKind kind = ClipKind::replay;
	std::string game;
	std::vector<ActivityFile> files;
	ActivityState state = ActivityState::waiting;
	ActivityClock::time_point started_at{};
	ActivityClock::time_point ended_at{};
};

class ActivityModel {
public:
	// Returns false for duplicate, unknown, stale, or out-of-order events.
	bool apply(const ActivityEvent &event);
	const std::deque<Activity> &activities() const;
	// The oldest moving/queued save, then the oldest waiting save, then the latest outcome.
	const Activity *current() const;
	// Cancels unfinished records and makes later callbacks inert. Idempotent.
	void stop(ActivityClock::time_point at = ActivityClock::now());

private:
	void trim_history();
	std::deque<Activity> activities_;
	std::uint64_t newest_id_ = 0;
	bool stopped_ = false;
};

} // namespace clip_atlas
