// Clip Atlas — one recording session: which game it belongs to and which files
// it wrote.
//
// A recording can outlast a game, start before one, or be split into several
// files by OBS ("Automatically split file" or the Split Recording hotkey). The
// tracker follows a session from RECORDING_STARTED to RECORDING_STOPPED so the
// glue can file every part into one game folder once the recording ends.
//
// The game:
//   - By default the game is decided when the recording starts: the game the
//     plugin would file a replay under at that moment (GameState::game_for_save,
//     so a game closed within the grace period still counts).
//   - With "dominant game" on, the game is the one that was current for the
//     longest total time while recording. Time with no game current does not
//     count for any game. If no game was ever current, the start game is used.
//
// The files: the first file is known at start; OBS's "file_changed" signal names
// each following file of a split recording ("next_file" is the NEW file). Paths
// are UTF-8 strings exactly as OBS gives them. Empty paths are ignored, and a
// path already in the session is not added twice.
//
// Games are told apart by ClipGame::key, or by ClipGame::name when the key is
// empty (a game with no known executable). An unsorted ClipGame is no game.
//
// Only std::bad_alloc can escape. If it does, the session may be left
// partly updated (out of memory only).
//
// Not thread-safe: the plugin only touches it on the UI thread. Time is passed
// in by the caller so tests control it.
//
// Pure C++17 with no OBS dependency. All strings are UTF-8.
#pragma once

#include "clip-routing.hpp"
#include "game-state.hpp"

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace clip_atlas {

// Identifies one session, so a late signal from an earlier recording (queued
// before it stopped, delivered after a new one started) is not mixed into the
// next one. Never 0 for a real session.
using RecordingSessionId = std::uint64_t;

struct FinishedRecording {
	ClipGame game;                  // the folder every file goes in; unsorted if empty
	std::vector<std::string> files; // in the order OBS wrote them, no duplicates, no empty paths
};

class RecordingTracker {
public:
	// A recording started. `first_file` is the file OBS is writing now (may be
	// empty if unknown). `start_game` is the game the recording is filed under
	// by default. `current_game` is the game current right now (unsorted if
	// none) and starts the dominant-game clock. A session already active is
	// discarded and replaced (its files are forgotten, not returned). Returns
	// the new session's id; ids increase with each start.
	RecordingSessionId start(const std::string &first_file, const ClipGame &start_game,
				 const ClipGame &current_game, SteadyClock::time_point now);

	// OBS moved on to `next_file`. Ignored unless `session_id` is the active
	// session. Returns whether the file was added.
	bool file_changed(RecordingSessionId session_id, const std::string &next_file);

	// The current game changed (unsorted if none). Closes the running segment
	// at `now` and adds its length to the previous game's total (unsorted time
	// is not counted), then starts a segment for `current_game`. A change to the
	// same game closes and reopens its segment, so no time is lost or counted
	// twice, and updates the key and name remembered for it.
	// Ignored when no session is active. A `now` earlier than the segment's
	// start counts as zero time, never negative, and the next segment starts
	// at that earlier `now` (the plugin passes SteadyClock::now(), which never
	// goes backwards).
	void game_changed(const ClipGame &current_game, SteadyClock::time_point now);

	// The recording stopped. Closes the running segment at `now` and ends the
	// session. `last_file` (OBS's last recording path) is added only when the
	// session has no files yet: the session's own list (first file + every
	// file_changed) is complete, while OBS's path can be overwritten by an
	// unrelated output (OBS 31's multitrack stream dump also sets it).
	//   - use_dominant false: game = the session's start_game.
	//   - use_dominant true: game = the game with the largest total time; on a
	//     tie, the one first current earliest in the recording (an exact tie on
	//     both goes to the identity that sorts first: deterministic but
	//     arbitrary). Its name is the latest name seen for it, its key the key
	//     it was seen with. A game current for zero time never wins.
	//     No time for any game: start_game.
	// With no active session (STOPPED without STARTED), returns
	// {fallback_game, {last_file}} (or no files if last_file is empty).
	FinishedRecording stop(const std::string &last_file, bool use_dominant, const ClipGame &fallback_game,
			       SteadyClock::time_point now);

	bool active() const;
	// 0 when no session is active.
	RecordingSessionId session() const;

private:
	struct GameTime {
		std::chrono::steady_clock::duration total{};
		SteadyClock::time_point first_seen{};
		ClipGame game; // latest key and name seen for this game
	};

	void close_segment(SteadyClock::time_point now);

	RecordingSessionId next_id_ = 1;
	RecordingSessionId session_ = 0;
	ClipGame start_game_;
	std::vector<std::string> files_;
	ClipGame segment_game_;
	SteadyClock::time_point segment_start_{};
	// By identity, "k:" + key or "n:" + name when the key is empty; games only.
	std::map<std::string, GameTime> game_times_;
};

} // namespace clip_atlas
