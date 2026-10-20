#include "game-state.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>
#include <string>

using namespace clip_atlas;

namespace {

// Fixed origin, so no test reads the real clock.
const SteadyClock::time_point kT0{};

SteadyClock::time_point at(int s)
{
	return kT0 + std::chrono::seconds(s);
}

SteadyClock::time_point at_ms(int ms)
{
	return kT0 + std::chrono::milliseconds(ms);
}

// Window details as the "hooked" signal reports them. Tests that need a different window for the same source change
// one field at a time.
HookInfo window(const std::string &title, const std::string &executable = "game.exe",
		const std::string &window_class = "UnityWndClass")
{
	HookInfo info;
	info.title = title;
	info.window_class = window_class;
	info.executable = executable;
	return info;
}

// Source id of a game, or "" for no game. No test uses an empty id, so "" always means none.
std::string source_of(const std::optional<CurrentGame> &game)
{
	return game ? game->source_id : std::string();
}

HookInfo info_of(const std::optional<CurrentGame> &game)
{
	return game ? game->info : HookInfo{};
}

} // namespace

TEST_CASE("a single hooked source is the current game until it unhooks")
{
	GameState state;
	CHECK_FALSE(state.current().has_value());
	CHECK_FALSE(state.no_game_since().has_value());

	CHECK(state.hooked("a", window("Celeste"), at(0)));
	CHECK(source_of(state.current()) == "a");
	CHECK(info_of(state.current()) == window("Celeste"));
	CHECK_FALSE(state.no_game_since().has_value());

	CHECK(state.unhooked("a", at(5)));
	CHECK_FALSE(state.current().has_value());
	REQUIRE(state.no_game_since().has_value());
	CHECK(*state.no_game_since() == at(5));
}

TEST_CASE("the most recently hooked source wins when both are hooked")
{
	GameState state;
	state.hooked("a", window("Alyx"), at(0));
	CHECK(state.hooked("b", window("Celeste"), at(1)));
	CHECK(source_of(state.current()) == "b");

	// Repeating the winner's hook with the same window changes nothing.
	CHECK_FALSE(state.hooked("b", window("Celeste"), at(2)));
	CHECK(source_of(state.current()) == "b");
}

TEST_CASE("a live source beats a more recently hooked one that is not live")
{
	GameState state;
	state.hooked("a", window("Alyx"), at(0));
	state.hooked("b", window("Celeste"), at(1));
	CHECK(state.source_live("a", true, at(2)));
	CHECK(source_of(state.current()) == "a");

	// b is hooked more recently but is not live, so re-hooking it does not take the game back.
	CHECK_FALSE(state.hooked("b", window("Celeste"), at(3)));
	CHECK(source_of(state.current()) == "a");
}

TEST_CASE("toggling liveness flips the winner between two hooked sources")
{
	GameState state;
	state.hooked("a", window("Alyx"), at(0));
	state.hooked("b", window("Celeste"), at(1));
	CHECK(source_of(state.current()) == "b");

	CHECK(state.source_live("a", true, at(2)));
	CHECK(source_of(state.current()) == "a");

	// Both live: the more recent hook (b) wins.
	CHECK(state.source_live("b", true, at(3)));
	CHECK(source_of(state.current()) == "b");

	// b leaves the live output, so the live a wins again.
	CHECK(state.source_live("b", false, at(4)));
	CHECK(source_of(state.current()) == "a");

	CHECK_FALSE(state.source_live("b", false, at(5)));
	CHECK(source_of(state.current()) == "a");
}

TEST_CASE("among live sources the most recent hook wins")
{
	GameState state;
	state.source_live("a", true, at(0));
	state.source_live("b", true, at(1));
	state.hooked("a", window("Alyx"), at(2));
	state.hooked("b", window("Celeste"), at(3));
	CHECK(source_of(state.current()) == "b");

	CHECK(state.hooked("a", window("Alyx"), at(4)));
	CHECK(source_of(state.current()) == "a");
}

TEST_CASE("re-hooking the older source makes it the most recent")
{
	GameState state;
	state.hooked("a", window("Alyx"), at(0));
	state.hooked("b", window("Celeste"), at(1));
	CHECK(state.hooked("a", window("Alyx"), at(2)));
	CHECK(source_of(state.current()) == "a");

	CHECK(state.unhooked("a", at(3)));
	CHECK(source_of(state.current()) == "b");
}

TEST_CASE("liveness reported before a hook is remembered")
{
	GameState state;
	CHECK_FALSE(state.source_live("a", true, at(0)));
	CHECK(state.hooked("a", window("Alyx"), at(1)));
	CHECK(source_of(state.current()) == "a");

	// b is hooked after a, so it is the most recent. a is live and b is not, so a keeps the game.
	CHECK_FALSE(state.hooked("b", window("Celeste"), at(2)));
	CHECK(source_of(state.current()) == "a");
}

TEST_CASE("liveness survives an unhook until the source is removed")
{
	GameState state;
	state.hooked("a", window("Alyx"), at(0));
	state.source_live("a", true, at(1));
	CHECK(state.unhooked("a", at(2)));
	CHECK(state.hooked("a", window("Alyx"), at(3)));
	CHECK(source_of(state.current()) == "a");

	// b is more recent than a's last hook, but a is still live.
	CHECK_FALSE(state.hooked("b", window("Celeste"), at(4)));
	CHECK(source_of(state.current()) == "a");
}

TEST_CASE("removing a source forgets its liveness")
{
	GameState state;
	state.source_live("a", true, at(0));
	CHECK_FALSE(state.source_removed("a", at(1)));
	CHECK(state.hooked("a", window("Alyx"), at(2)));
	CHECK(state.hooked("b", window("Celeste"), at(3)));
	CHECK(source_of(state.current()) == "b");
}

TEST_CASE("removing the current source falls back to the other hooked source")
{
	GameState state;
	state.hooked("a", window("Alyx"), at(0));
	state.hooked("b", window("Celeste"), at(1));
	CHECK(state.source_removed("b", at(2)));
	CHECK(source_of(state.current()) == "a");
	CHECK_FALSE(state.no_game_since().has_value());

	CHECK(state.source_removed("a", at(3)));
	CHECK_FALSE(state.current().has_value());
	REQUIRE(state.no_game_since().has_value());
	CHECK(*state.no_game_since() == at(3));
}

TEST_CASE("removing a source that is not current leaves the game alone")
{
	GameState state;
	state.hooked("a", window("Alyx"), at(0));
	state.hooked("b", window("Celeste"), at(1));
	CHECK_FALSE(state.source_removed("a", at(2)));
	CHECK(source_of(state.current()) == "b");
}

TEST_CASE("unhooking a source that is not current leaves the game alone")
{
	GameState state;
	state.hooked("a", window("Alyx"), at(0));
	state.hooked("b", window("Celeste"), at(1));
	CHECK_FALSE(state.unhooked("a", at(2)));
	CHECK(source_of(state.current()) == "b");

	CHECK(state.unhooked("b", at(3)));
	CHECK_FALSE(state.current().has_value());
}

TEST_CASE("clear() makes the cleared game the last seen one")
{
	GameState state;
	state.hooked("a", window("Alyx"), at(0));
	state.hooked("b", window("Celeste"), at(1));
	CHECK(state.clear(at(10)));
	CHECK_FALSE(state.current().has_value());
	REQUIRE(state.no_game_since().has_value());
	CHECK(*state.no_game_since() == at(10));

	CHECK(source_of(state.game_for_save(at(12))) == "b");
	CHECK(info_of(state.game_for_save(at(12))) == window("Celeste"));

	// Nothing is current, so a second clear changes nothing. It must not restart the gap or the grace window.
	CHECK_FALSE(state.clear(at(12)));
	CHECK(*state.no_game_since() == at(10));
	CHECK(source_of(state.game_for_save(at(12))) == "b");
}

TEST_CASE("the last seen game is returned up to and including the grace period")
{
	GameState state(std::chrono::seconds(10));
	state.hooked("a", window("Alyx"), at(0));
	state.unhooked("a", at(100));

	CHECK(source_of(state.game_for_save(at(100))) == "a");
	CHECK(source_of(state.game_for_save(at(110))) == "a");
	CHECK_FALSE(state.game_for_save(at(110) + std::chrono::milliseconds(1)).has_value());
}

TEST_CASE("the grace boundary is exact to the nanosecond")
{
	GameState state(std::chrono::seconds(10));
	state.hooked("a", window("Alyx"), at(0));
	state.unhooked("a", at(100));

	CHECK(source_of(state.game_for_save(at(110) - std::chrono::nanoseconds(1))) == "a");
	CHECK_FALSE(state.game_for_save(at(110) + std::chrono::nanoseconds(1)).has_value());
}

TEST_CASE("a zero grace period covers only the instant the game went away")
{
	GameState state(std::chrono::milliseconds(0));
	state.hooked("a", window("Alyx"), at(0));
	state.unhooked("a", at(5));

	CHECK(source_of(state.game_for_save(at(5))) == "a");
	CHECK_FALSE(state.game_for_save(at(5) + std::chrono::milliseconds(1)).has_value());
}

TEST_CASE("a negative grace period is clamped to zero")
{
	GameState state(std::chrono::milliseconds(-5000));
	CHECK(state.grace_period() == std::chrono::milliseconds(0));

	state.hooked("a", window("Alyx"), at(0));
	state.unhooked("a", at(5));
	CHECK(source_of(state.game_for_save(at(5))) == "a");
	CHECK_FALSE(state.game_for_save(at(5) + std::chrono::milliseconds(1)).has_value());

	state.set_grace_period(std::chrono::milliseconds(-1));
	CHECK(state.grace_period() == std::chrono::milliseconds(0));
}

TEST_CASE("set_grace_period changes the window that game_for_save uses")
{
	GameState state;
	CHECK(state.grace_period() == std::chrono::seconds(10));

	state.hooked("a", window("Alyx"), at(0));
	state.unhooked("a", at(100));
	state.set_grace_period(std::chrono::seconds(2));
	CHECK(state.grace_period() == std::chrono::seconds(2));
	CHECK(source_of(state.game_for_save(at(102))) == "a");
	CHECK_FALSE(state.game_for_save(at(102) + std::chrono::milliseconds(1)).has_value());
}

TEST_CASE("a very large grace period is honoured without overflow")
{
	GameState state(std::chrono::milliseconds::max());
	CHECK(state.grace_period() == std::chrono::milliseconds::max());

	state.hooked("a", window("Alyx"), at(0));
	state.unhooked("a", at(5));
	CHECK(source_of(state.game_for_save(at(1000000))) == "a");
}

TEST_CASE("game_for_save is empty with no last seen game or before it")
{
	GameState state;
	CHECK_FALSE(state.game_for_save(at(0)).has_value());

	state.hooked("a", window("Alyx"), at(10));
	state.unhooked("a", at(100));
	// A clock that moved backwards must not bring the game back.
	CHECK_FALSE(state.game_for_save(at(99)).has_value());
}

TEST_CASE("game_for_save returns the current game and ignores the last seen one")
{
	GameState state;
	state.hooked("a", window("Alyx"), at(0));
	state.hooked("b", window("Celeste"), at(100));

	// a is within grace as the last seen game, but b is current.
	CHECK(source_of(state.game_for_save(at(105))) == "b");
	CHECK(source_of(state.game_for_save(at(100000))) == "b");
}

TEST_CASE("the last seen game is the one that ended most recently")
{
	GameState state;
	state.hooked("a", window("Alyx"), at(0));
	state.hooked("b", window("Celeste"), at(10));
	state.unhooked("b", at(20));
	state.unhooked("a", at(30));

	CHECK(source_of(state.game_for_save(at(35))) == "a");
	CHECK_FALSE(state.game_for_save(at(45)).has_value());
}

TEST_CASE("no_game_since counts from when the last game went away")
{
	GameState state;
	state.hooked("a", window("Alyx"), at(0));
	state.hooked("b", window("Celeste"), at(10));
	CHECK_FALSE(state.no_game_since().has_value());

	state.unhooked("b", at(20));
	CHECK_FALSE(state.no_game_since().has_value());

	state.unhooked("a", at(30));
	REQUIRE(state.no_game_since().has_value());
	CHECK(*state.no_game_since() == at(30));

	CHECK_FALSE(state.clear(at(40)));
	CHECK(*state.no_game_since() == at(30));

	state.hooked("c", window("Desktop"), at(50));
	CHECK_FALSE(state.no_game_since().has_value());
}

TEST_CASE("mutators return true only when current() changed")
{
	GameState state;
	CHECK(state.hooked("a", window("Alyx", "alyx.exe"), at(0)));
	// Same source and window, and it is already the winner.
	CHECK_FALSE(state.hooked("a", window("Alyx", "alyx.exe"), at(1)));
	// Same source with a new title: current() differs in info, so this counts.
	CHECK(state.hooked("a", window("Alyx - Menu", "alyx.exe"), at(2)));
	CHECK(info_of(state.current()).title == "Alyx - Menu");

	CHECK(state.hooked("b", window("Celeste"), at(3)));
	CHECK_FALSE(state.hooked("b", window("Celeste"), at(4)));
	CHECK(state.hooked("a", window("Alyx - Menu", "alyx.exe"), at(5)));
	CHECK(source_of(state.current()) == "a");

	CHECK(state.unhooked("a", at(6)));
	CHECK(source_of(state.current()) == "b");
	CHECK_FALSE(state.unhooked("a", at(7)));
	CHECK(state.unhooked("b", at(8)));
	CHECK_FALSE(state.unhooked("b", at(9)));

	CHECK_FALSE(state.source_live("a", true, at(10)));
	CHECK_FALSE(state.source_removed("a", at(11)));
	CHECK_FALSE(state.clear(at(12)));

	CHECK(state.hooked("c", window("Desktop"), at(13)));
	CHECK(state.clear(at(14)));
	CHECK_FALSE(state.clear(at(15)));
}

TEST_CASE("unhooked and source_removed ignore unknown ids")
{
	GameState state;
	CHECK_FALSE(state.unhooked("ghost", at(0)));
	CHECK_FALSE(state.source_removed("ghost", at(1)));
	CHECK_FALSE(state.current().has_value());
	CHECK_FALSE(state.no_game_since().has_value());

	state.hooked("a", window("Alyx"), at(2));
	CHECK_FALSE(state.unhooked("ghost", at(3)));
	CHECK_FALSE(state.source_removed("ghost", at(4)));
	CHECK(source_of(state.current()) == "a");
}

TEST_CASE("two sources with identical window info are told apart by source id")
{
	GameState state;
	CHECK(state.hooked("a", window("Alyx", "alyx.exe"), at(0)));
	// Same title, window class and executable, but a different source, so the current game still changes.
	CHECK(state.hooked("b", window("Alyx", "alyx.exe"), at(1)));
	CHECK(source_of(state.current()) == "b");

	CHECK(state.source_removed("b", at(2)));
	CHECK(source_of(state.current()) == "a");

	CHECK(state.source_removed("a", at(3)));
	CHECK_FALSE(state.current().has_value());
}

TEST_CASE("clear() forgets liveness, so a later hook is not live")
{
	GameState state;
	CHECK_FALSE(state.source_live("a", true, at(0)));
	CHECK_FALSE(state.clear(at(1)));

	CHECK(state.hooked("a", window("Alyx"), at(2)));
	CHECK(state.hooked("b", window("Celeste"), at(3)));
	CHECK(source_of(state.current()) == "b");
}

TEST_CASE("a re-hook that changes only the executable or the window class is a change")
{
	GameState state;
	CHECK(state.hooked("a", window("Alyx", "alyx.exe"), at(0)));

	CHECK(state.hooked("a", window("Alyx", "launcher.exe"), at(1)));
	CHECK(info_of(state.current()) == window("Alyx", "launcher.exe"));

	CHECK(state.hooked("a", window("Alyx", "launcher.exe", "GLFW30"), at(2)));
	CHECK(info_of(state.current()).window_class == "GLFW30");
	CHECK(info_of(state.current()) == window("Alyx", "launcher.exe", "GLFW30"));
}

TEST_CASE("HookInfo equality and inequality compare title, window class and executable")
{
	const HookInfo base = window("Alyx", "alyx.exe", "UnityWndClass");
	CHECK(base == window("Alyx", "alyx.exe", "UnityWndClass"));
	CHECK_FALSE(base != window("Alyx", "alyx.exe", "UnityWndClass"));

	CHECK(base != window("Alyx - Menu", "alyx.exe", "UnityWndClass"));
	CHECK(base != window("Alyx", "launcher.exe", "UnityWndClass"));
	CHECK(base != window("Alyx", "alyx.exe", "GLFW30"));
}

TEST_CASE("grace after a fallback runs from when the last game went away")
{
	GameState state;
	CHECK(state.hooked("a", window("Alyx"), at(0)));
	CHECK(state.hooked("b", window("Celeste"), at(10)));
	// Falls back to a, which is still a game.
	CHECK(state.source_removed("b", at(20)));
	// The last game goes away here, so the grace period starts now and not at the fallback.
	CHECK(state.unhooked("a", at(30)));

	CHECK(source_of(state.game_for_save(at(39))) == "a"); // 9 s later: within grace
	CHECK_FALSE(state.game_for_save(at(41)).has_value()); // 11 s later: expired
}

TEST_CASE("a game that already went away stays last seen after its source is removed")
{
	GameState state;
	CHECK(state.hooked("a", window("Alyx"), at_ms(0)));
	CHECK(state.unhooked("a", at_ms(100)));
	CHECK_FALSE(state.source_removed("a", at_ms(101)));

	REQUIRE(state.no_game_since().has_value());
	CHECK(*state.no_game_since() == at_ms(100));
	CHECK(source_of(state.game_for_save(at_ms(105))) == "a");
}

TEST_CASE("source_removed while current makes that game the last seen one")
{
	GameState state;
	CHECK(state.hooked("a", window("Alyx"), at(0)));
	CHECK(state.hooked("b", window("Celeste"), at(1)));
	CHECK(state.source_removed("b", at(2)));
	// Nothing is left, and a was the current game when it went.
	CHECK(state.source_removed("a", at(3)));

	CHECK_FALSE(state.current().has_value());
	CHECK(source_of(state.game_for_save(at(4))) == "a");
}
