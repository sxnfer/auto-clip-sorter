#include "replay-policy.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace clip_atlas;

namespace {

using Clock = ReplayPolicyInput::TimePoint;
using std::chrono::milliseconds;
using std::chrono::seconds;

Clock clock_at(seconds s)
{
	return Clock(s);
}

// A game is current and nothing blocks starting the Replay Buffer.
ReplayPolicyInput start_ready()
{
	ReplayPolicyInput in;
	in.game_current = true;
	return in;
}

// The decision must be "do nothing, and don't come back".
void expect_nothing(const ReplayPolicyInput &in)
{
	const ReplayDecision d = decide_replay_action(in);
	CHECK(d.action == ReplayAction::none);
	CHECK_FALSE(d.recheck_at.has_value());
}

} // namespace

TEST_CASE("decide_replay_action: starts the buffer when a game is current and nothing blocks it")
{
	const ReplayDecision d = decide_replay_action(start_ready());
	CHECK(d.action == ReplayAction::start);
	CHECK_FALSE(d.recheck_at.has_value());
}

TEST_CASE("decide_replay_action: each start condition blocks starting on its own")
{
	SECTION("auto start is off")
	{
		ReplayPolicyInput in = start_ready();
		in.auto_start = false;
		expect_nothing(in);
	}
	SECTION("the Replay Buffer is disabled in Output settings")
	{
		ReplayPolicyInput in = start_ready();
		in.buffer_enabled = false;
		expect_nothing(in);
	}
	SECTION("no game is current")
	{
		ReplayPolicyInput in = start_ready();
		in.game_current = false;
		expect_nothing(in);
	}
	SECTION("the buffer is already running, started by the user")
	{
		ReplayPolicyInput in = start_ready();
		in.buffer_active = true;
		expect_nothing(in);
	}
	SECTION("the buffer is already running, started by us")
	{
		ReplayPolicyInput in = start_ready();
		in.buffer_active = true;
		in.we_started = true;
		expect_nothing(in);
	}
	SECTION("a request to OBS is still pending")
	{
		ReplayPolicyInput in = start_ready();
		in.request_pending = true;
		expect_nothing(in);
	}
	SECTION("the user stopped the buffer during this game")
	{
		ReplayPolicyInput in = start_ready();
		in.suppressed = true;
		expect_nothing(in);
	}
}

TEST_CASE("decide_replay_action: a buffer we did not start is never stopped")
{
	const Clock since = clock_at(seconds(1000));
	ReplayPolicyInput in;
	in.buffer_active = true; // running, but the user started it, so we_started stays false
	in.no_game_since = since;
	in.grace_period = seconds(10);
	in.now = since + seconds(3600);

	// Same state, but ours: this proves the flag is the only thing keeping the buffer alive.
	ReplayPolicyInput ours = in;
	ours.we_started = true;
	CHECK(decide_replay_action(ours).action == ReplayAction::stop);

	SECTION("with the grace period long elapsed")
	{
		expect_nothing(in);
	}
	SECTION("at the exact moment the grace period ends")
	{
		in.now = since + seconds(10);
		expect_nothing(in);
	}
	SECTION("with no game ever seen")
	{
		in.no_game_since.reset();
		expect_nothing(in);
	}
	SECTION("with zero grace")
	{
		in.grace_period = milliseconds(0);
		in.now = since;
		expect_nothing(in);
	}
	SECTION("in a fresh input, where we_started defaults to false")
	{
		ReplayPolicyInput fresh;
		fresh.buffer_active = true;
		fresh.no_game_since = since;
		fresh.now = since + seconds(3600);
		CHECK_FALSE(fresh.we_started);
		expect_nothing(fresh);
	}
}

TEST_CASE("decide_replay_action: no combination of flags stops a buffer we did not start")
{
	const Clock since = clock_at(seconds(1000));
	const seconds moments[] = {seconds(5), seconds(10), seconds(3600)}; // before, at and after the deadline
	int stops_theirs = 0;
	int stops_ours = 0;
	for (int we_started = 0; we_started < 2; ++we_started) {
		for (int bits = 0; bits < 128; ++bits) {
			for (const seconds moment : moments) {
				for (int with_since = 0; with_since < 2; ++with_since) {
					ReplayPolicyInput in;
					in.auto_start = (bits & 1) != 0;
					in.auto_stop = (bits & 2) != 0;
					in.buffer_enabled = (bits & 4) != 0;
					in.game_current = (bits & 8) != 0;
					in.buffer_active = (bits & 16) != 0;
					in.request_pending = (bits & 32) != 0;
					in.suppressed = (bits & 64) != 0;
					in.we_started = we_started != 0;
					in.grace_period = seconds(10);
					in.now = since + moment;
					if (with_since != 0)
						in.no_game_since = since;

					if (decide_replay_action(in).action == ReplayAction::stop) {
						if (in.we_started)
							++stops_ours;
						else
							++stops_theirs;
					}
				}
			}
		}
	}
	CHECK(stops_theirs == 0);
	// Guards against the sweep never reaching the stop path, which would make the zero above meaningless.
	CHECK(stops_ours > 0);
}

TEST_CASE("decide_replay_action: a stop needs auto stop, no pending request and no current game")
{
	const Clock since = clock_at(seconds(1000));
	ReplayPolicyInput in;
	in.buffer_active = true;
	in.we_started = true;
	in.no_game_since = since;
	in.grace_period = seconds(10);
	in.now = since + seconds(60); // grace long elapsed, so only the flag under test can block the stop
	CHECK(decide_replay_action(in).action == ReplayAction::stop);

	SECTION("auto stop is off")
	{
		in.auto_stop = false;
		expect_nothing(in);
	}
	SECTION("OBS has not answered the last request")
	{
		in.request_pending = true;
		expect_nothing(in);
	}
	SECTION("a game is current again")
	{
		in.game_current = true;
		expect_nothing(in);
	}
	SECTION("the buffer is not running")
	{
		in.buffer_active = false;
		expect_nothing(in);
	}
}

TEST_CASE("decide_replay_action: a stop waits out the grace period, then happens at the deadline")
{
	const Clock since = clock_at(seconds(1000));
	const Clock deadline = since + seconds(10);
	ReplayPolicyInput in;
	in.buffer_active = true;
	in.we_started = true;
	in.no_game_since = since;
	in.grace_period = seconds(10);

	in.now = since + seconds(4);
	ReplayDecision d = decide_replay_action(in);
	CHECK(d.action == ReplayAction::none);
	REQUIRE(d.recheck_at.has_value());
	CHECK(*d.recheck_at == deadline);

	in.now = deadline - milliseconds(1);
	d = decide_replay_action(in);
	CHECK(d.action == ReplayAction::none);
	REQUIRE(d.recheck_at.has_value());
	CHECK(*d.recheck_at == deadline);

	in.now = deadline;
	d = decide_replay_action(in);
	CHECK(d.action == ReplayAction::stop);
	CHECK_FALSE(d.recheck_at.has_value());

	in.now = deadline + seconds(1);
	CHECK(decide_replay_action(in).action == ReplayAction::stop);
}

TEST_CASE("decide_replay_action: a game returning during the grace period cancels the pending stop")
{
	const Clock since = clock_at(seconds(1000));
	ReplayPolicyInput in;
	in.buffer_active = true;
	in.we_started = true;
	in.no_game_since = since;
	in.grace_period = seconds(10);

	in.now = since + seconds(4);
	REQUIRE(decide_replay_action(in).recheck_at.has_value()); // the stop is pending

	// The game is back before the deadline. The old deadline passing must not stop the buffer.
	in.game_current = true;
	in.now = since + seconds(20);
	expect_nothing(in);
}

TEST_CASE("decide_replay_action: a missing no_game_since counts as grace already elapsed")
{
	ReplayPolicyInput in;
	in.buffer_active = true;
	in.we_started = true;
	in.grace_period = seconds(10);
	in.now = clock_at(seconds(5)); // no_game_since stays nullopt: no game was ever seen

	const ReplayDecision d = decide_replay_action(in);
	CHECK(d.action == ReplayAction::stop);
	CHECK_FALSE(d.recheck_at.has_value());
}

TEST_CASE("decide_replay_action: zero grace stops as soon as the game is gone")
{
	const Clock since = clock_at(seconds(1000));
	ReplayPolicyInput in;
	in.buffer_active = true;
	in.we_started = true;
	in.no_game_since = since;
	in.grace_period = milliseconds(0);

	in.now = since;
	CHECK(decide_replay_action(in).action == ReplayAction::stop);
	in.now = since + milliseconds(1);
	CHECK(decide_replay_action(in).action == ReplayAction::stop);
}

TEST_CASE("decide_replay_action: a negative grace period is treated as zero")
{
	const Clock since = clock_at(seconds(1000));
	ReplayPolicyInput in;
	in.buffer_active = true;
	in.we_started = true;
	in.no_game_since = since;
	in.grace_period = seconds(-10);

	in.now = since;
	CHECK(decide_replay_action(in).action == ReplayAction::stop);

	// Zero, not minus ten seconds: the recheck falls at the moment the game left.
	in.now = since - milliseconds(1);
	const ReplayDecision d = decide_replay_action(in);
	CHECK(d.action == ReplayAction::none);
	REQUIRE(d.recheck_at.has_value());
	CHECK(*d.recheck_at == since);
}

TEST_CASE("decide_replay_action: an absurd grace period does not wrap into the past")
{
	const Clock since = clock_at(seconds(1000));
	ReplayPolicyInput in;
	in.buffer_active = true;
	in.we_started = true;
	in.no_game_since = since;
	in.grace_period = milliseconds::max();
	in.now = since + seconds(3600);

	const ReplayDecision d = decide_replay_action(in);
	CHECK(d.action == ReplayAction::none);
	REQUIRE(d.recheck_at.has_value());
	CHECK(*d.recheck_at > in.now);
}

TEST_CASE("decide_replay_action: nothing to do means no action and no recheck")
{
	SECTION("idle: no game and no buffer")
	{
		expect_nothing(ReplayPolicyInput{});
	}
	SECTION("a game is current and our buffer is already running")
	{
		ReplayPolicyInput in = start_ready();
		in.buffer_active = true;
		in.we_started = true;
		expect_nothing(in);
	}
	SECTION("the game left, the buffer is off, and we_started is stale")
	{
		ReplayPolicyInput in;
		in.we_started = true;
		in.no_game_since = clock_at(seconds(1000));
		in.now = clock_at(seconds(9999));
		expect_nothing(in);
	}
}
