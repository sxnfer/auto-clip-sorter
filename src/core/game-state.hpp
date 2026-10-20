// Clip Atlas — which game is "current", given hook events from any number of
// capture sources.
//
// Rules:
//   - Among hooked sources, one that is live in the Program output wins over
//     one that is not. Ties go to the most recently hooked.
//   - When the current game goes away (unhook, removal, or another source
//     taking over), it is remembered with a timestamp so a replay saved a few
//     seconds after the game closed still lands in that game's folder.
//
// Time is passed in by the caller so tests control it. Not thread-safe: the
// plugin only touches it on the UI thread.
//
// Pure C++17 with no OBS dependency. All strings are UTF-8.
#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace clip_atlas {

using SteadyClock = std::chrono::steady_clock;

// Copied out of the "hooked" signal calldata.
struct HookInfo {
	std::string title;
	std::string window_class;
	std::string executable;

	bool operator==(const HookInfo &other) const
	{
		return title == other.title && window_class == other.window_class && executable == other.executable;
	}
	bool operator!=(const HookInfo &other) const { return !(*this == other); }
};

struct CurrentGame {
	std::string source_id; // the capture source's UUID
	HookInfo info;
};

class GameState {
public:
	explicit GameState(std::chrono::milliseconds grace_period = std::chrono::seconds(10));

	void set_grace_period(std::chrono::milliseconds grace_period);
	std::chrono::milliseconds grace_period() const;

	// Each mutator returns true if current() changed as a result, comparing
	// both source_id and info.

	// Source started capturing a window. A repeat hook on an already hooked
	// source updates its info and counts as the most recent hook.
	bool hooked(const std::string &source_id, const HookInfo &info, SteadyClock::time_point now);
	// Source stopped capturing. No-op for a source that isn't hooked.
	bool unhooked(const std::string &source_id, SteadyClock::time_point now);
	// Source became visible (true) or hidden (false) in the Program output.
	// Liveness is tracked even while the source is not hooked, so a later
	// hook knows whether it is live.
	bool source_live(const std::string &source_id, bool live, SteadyClock::time_point now);
	// Source was destroyed or stopped being tracked. Forgets its entry (hook and
	// liveness). A game that already went away stays the last-seen game, so a save
	// in the grace period after its source was deleted still finds it.
	bool source_removed(const std::string &source_id, SteadyClock::time_point now);
	// Forgets every source, e.g. when the scene collection changes. The
	// current game, if any, becomes the last-seen game.
	bool clear(SteadyClock::time_point now);

	// The winning hooked source, if any.
	std::optional<CurrentGame> current() const;

	// Game a clip saved at `now` belongs to: current() if there is one,
	// otherwise the last-seen game if it went away no more than
	// grace_period() before `now` (inclusive). nullopt means Unsorted.
	std::optional<CurrentGame> game_for_save(SteadyClock::time_point now) const;

	// When current() last went from a game to no game. nullopt while a game
	// is current or if there has never been one. Replay-policy uses this to
	// time the Replay Buffer stop.
	std::optional<SteadyClock::time_point> no_game_since() const;

private:
	struct SourceEntry {
		bool hooked = false;
		bool live = false;
		HookInfo info;
		std::uint64_t hook_order = 0; // larger = hooked more recently
	};

	// Recomputes the winner after any change; records last-seen/no_game_since.
	bool update_current(SteadyClock::time_point now);

	std::chrono::milliseconds grace_period_;
	std::map<std::string, SourceEntry> sources_;
	std::uint64_t next_hook_order_ = 1;
	std::optional<CurrentGame> current_;
	std::optional<CurrentGame> last_seen_;
	SteadyClock::time_point last_seen_at_{};
	std::optional<SteadyClock::time_point> no_game_since_;
};

} // namespace clip_atlas
