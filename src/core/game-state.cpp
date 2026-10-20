#include "game-state.hpp"

#include <chrono>
#include <utility>

namespace clip_atlas {

namespace {

// A negative grace period would expire every last-seen game at once. Clamping to zero keeps the instant the game
// went away inside the window.
std::chrono::milliseconds clamp_grace(std::chrono::milliseconds grace)
{
	return grace < std::chrono::milliseconds::zero() ? std::chrono::milliseconds::zero() : grace;
}

// Two values name the same game only if both the source and the window match. A new window on the same source
// is still a change to current(), so the mutators report it.
bool same_game(const std::optional<CurrentGame> &a, const std::optional<CurrentGame> &b)
{
	if (!a || !b)
		return a.has_value() == b.has_value();
	return a->source_id == b->source_id && a->info == b->info;
}

} // namespace

GameState::GameState(std::chrono::milliseconds grace) : grace_period_(clamp_grace(grace)) {}

void GameState::set_grace_period(std::chrono::milliseconds grace)
{
	grace_period_ = clamp_grace(grace);
}

std::chrono::milliseconds GameState::grace_period() const
{
	return grace_period_;
}

bool GameState::hooked(const std::string &source_id, const HookInfo &info, SteadyClock::time_point now)
{
	// Copy before touching the entry. If the copy throws, the entry is still in its old state.
	HookInfo copy = info;
	SourceEntry &entry = sources_[source_id];
	entry.hooked = true;
	entry.info = std::move(copy);
	// A repeat hook takes a new order, so the source counts as the most recent hook again.
	entry.hook_order = next_hook_order_++;
	return update_current(now);
}

bool GameState::unhooked(const std::string &source_id, SteadyClock::time_point now)
{
	auto it = sources_.find(source_id);
	if (it == sources_.end() || !it->second.hooked)
		return false;
	// The entry stays so liveness survives the unhook. A later hook of the same source is still live.
	it->second.hooked = false;
	it->second.info = HookInfo{};
	return update_current(now);
}

bool GameState::source_live(const std::string &source_id, bool live, SteadyClock::time_point now)
{
	// Liveness can be reported before the first hook. The entry stays unhooked until hooked() runs.
	sources_[source_id].live = live;
	return update_current(now);
}

bool GameState::source_removed(const std::string &source_id, SteadyClock::time_point now)
{
	sources_.erase(source_id);
	return update_current(now);
}

bool GameState::clear(SteadyClock::time_point now)
{
	sources_.clear();
	return update_current(now);
}

std::optional<CurrentGame> GameState::current() const
{
	return current_;
}

std::optional<CurrentGame> GameState::game_for_save(SteadyClock::time_point now) const
{
	if (current_)
		return current_;
	// A query from before the game ended (a clock that moved backwards) must not report it.
	if (!last_seen_ || now < last_seen_at_)
		return std::nullopt;
	// The grace period is whole milliseconds, so rounding the elapsed time up gives the same answer as an exact
	// comparison. It also never converts a huge grace period to nanoseconds, which would overflow.
	if (std::chrono::ceil<std::chrono::milliseconds>(now - last_seen_at_) > grace_period_)
		return std::nullopt;
	return last_seen_;
}

std::optional<SteadyClock::time_point> GameState::no_game_since() const
{
	return no_game_since_;
}

bool GameState::update_current(SteadyClock::time_point now)
{
	// A live source beats a non-live one. Among equals the largest hook_order wins, which is the most recent hook.
	std::optional<CurrentGame> next;
	const SourceEntry *best = nullptr;
	for (const auto &[source_id, entry] : sources_) {
		if (!entry.hooked)
			continue;
		const bool wins = !best ||
				  (entry.live != best->live ? entry.live : entry.hook_order > best->hook_order);
		if (wins) {
			best = &entry;
			next = CurrentGame{source_id, entry.info};
		}
	}

	if (same_game(next, current_))
		return false;

	const bool had_game = current_.has_value();
	// Keep the game that just ended, so a replay saved a few seconds later still goes into its folder.
	if (had_game) {
		last_seen_ = current_;
		last_seen_at_ = now;
	}
	// Replay Buffer stop is timed from the moment the last game went away, not from any later change.
	if (next) {
		no_game_since_.reset();
	} else if (had_game) {
		no_game_since_ = now;
	}
	current_ = std::move(next);
	return true;
}

} // namespace clip_atlas
