// Clip Atlas — when to start or stop the Replay Buffer automatically.
//
// One pure function: given a snapshot of the world, say what to do now and,
// if nothing, when to ask again. The replay-controller owns the timer and
// the OBS calls.
//
// The rule that matters most: only stop a Replay Buffer that we started. A
// buffer the user started by hand is theirs.
//
// Pure C++17 with no OBS dependency.
#pragma once

#include <chrono>
#include <optional>

namespace clip_atlas {

enum class ReplayAction {
	none,
	start,
	stop,
};

struct ReplayPolicyInput {
	using TimePoint = std::chrono::steady_clock::time_point;

	TimePoint now;

	// Settings.
	bool auto_start = true;
	bool auto_stop = true;
	std::chrono::milliseconds grace_period{10000};

	// From GameState: whether a game is current, and if not, since when
	// there has been no game (nullopt if there never was one).
	bool game_current = false;
	std::optional<TimePoint> no_game_since;

	// From OBS / the controller.
	bool buffer_enabled = true;   // Replay Buffer is enabled in Output settings
	bool buffer_active = false;   // obs_frontend_replay_buffer_active()
	bool we_started = false;      // the active buffer was started by us
	bool request_pending = false; // we asked OBS to start/stop and it hasn't answered yet
	// The user stopped a buffer while the current game was running. Stays set
	// until the current game changes, so we don't restart it behind their back.
	bool suppressed = false;
};

struct ReplayDecision {
	ReplayAction action = ReplayAction::none;
	// Set when `action` is none but the answer may change with time alone
	// (a pending stop waiting out the grace period). The controller calls
	// decide_replay_action() again at that time.
	std::optional<ReplayPolicyInput::TimePoint> recheck_at;
};

// start: auto_start && buffer_enabled && game_current && !buffer_active
//        && !request_pending && !suppressed.
// stop:  auto_stop && buffer_active && we_started && !game_current
//        && !request_pending && now >= no_game_since + grace_period.
//        Before that instant: none with recheck_at = no_game_since + grace.
//        A game becoming current again inside the grace period simply makes
//        the stop condition false, which cancels the pending stop.
//        A missing no_game_since with no current game counts as "grace
//        already elapsed".
// Otherwise none, with no recheck_at.
ReplayDecision decide_replay_action(const ReplayPolicyInput &input);

} // namespace clip_atlas
