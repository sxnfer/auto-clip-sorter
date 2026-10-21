#include "replay-policy.hpp"

#include <chrono>
#include <optional>

namespace clip_atlas {

namespace {

// Far longer than any grace period anyone would set. Larger values behave as this one, which
// also keeps `no_game_since + grace` from overflowing the clock: a wrapped deadline would lie in
// the past and stop the buffer at once.
constexpr std::chrono::milliseconds kMaxGracePeriod = std::chrono::hours(24 * 365);

std::chrono::milliseconds clamped_grace(std::chrono::milliseconds grace)
{
	if (grace < std::chrono::milliseconds::zero())
		return std::chrono::milliseconds::zero();
	if (grace > kMaxGracePeriod)
		return kMaxGracePeriod;
	return grace;
}

} // namespace

ReplayDecision decide_replay_action(const ReplayPolicyInput &in)
{
	// Start and stop cannot both hold: start needs a game, stop needs none.
	const bool may_start = in.auto_start && in.buffer_enabled && in.game_current && !in.buffer_active &&
			       !in.request_pending && !in.suppressed;
	if (may_start)
		return ReplayDecision{ReplayAction::start, std::nullopt};

	// Only a buffer we started is ours to stop. One the user started by hand keeps running,
	// whatever the other flags say.
	const bool may_stop = in.auto_stop && in.buffer_active && in.we_started && !in.game_current &&
			      !in.request_pending;
	if (!may_stop)
		return ReplayDecision{};

	// nullopt means no game was ever seen, so there is no grace period to wait for.
	if (!in.no_game_since)
		return ReplayDecision{ReplayAction::stop, std::nullopt};

	const ReplayPolicyInput::TimePoint deadline = *in.no_game_since + clamped_grace(in.grace_period);
	if (in.now >= deadline)
		return ReplayDecision{ReplayAction::stop, std::nullopt};

	// Not yet. The controller asks again at the deadline. A game returning before then makes
	// may_stop false, which cancels the pending stop without any extra bookkeeping.
	return ReplayDecision{ReplayAction::none, deadline};
}

} // namespace clip_atlas
