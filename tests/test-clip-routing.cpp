#include "clip-routing.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace clip_atlas;

namespace {

CurrentGame game(const std::string &exe, const std::string &title)
{
	CurrentGame g;
	g.source_id = "uuid-1";
	g.info.executable = exe;
	g.info.title = title;
	return g;
}

} // namespace

TEST_CASE("sorting_enabled needs the master switch and the kind's toggle")
{
	Settings s;
	CHECK(sorting_enabled(s, ClipKind::replay));
	CHECK(sorting_enabled(s, ClipKind::recording));
	CHECK(sorting_enabled(s, ClipKind::screenshot));

	s.sort_recordings = false;
	CHECK(sorting_enabled(s, ClipKind::replay));
	CHECK(!sorting_enabled(s, ClipKind::recording));
	CHECK(sorting_enabled(s, ClipKind::screenshot));

	s = Settings{};
	s.sort_replays = false;
	s.sort_screenshots = false;
	CHECK(!sorting_enabled(s, ClipKind::replay));
	CHECK(sorting_enabled(s, ClipKind::recording));
	CHECK(!sorting_enabled(s, ClipKind::screenshot));

	s = Settings{};
	s.auto_sort = false;
	CHECK(!sorting_enabled(s, ClipKind::replay));
	CHECK(!sorting_enabled(s, ClipKind::recording));
	CHECK(!sorting_enabled(s, ClipKind::screenshot));
}

TEST_CASE("user_game_rules keeps the first rule for an executable")
{
	const UserGameRules rules = user_game_rules({{"a.exe", "First", false}, {"a.exe", "Second", true}});
	REQUIRE(rules.size() == 1);
	CHECK(rules.at("a.exe").display_name == "First");
	CHECK(!rules.at("a.exe").ignore);
}

TEST_CASE("clip_game_for resolves the game or routes to unsorted")
{
	Settings s;
	CHECK(clip_game_for(std::nullopt, s).unsorted());
	CHECK(clip_game_for(std::nullopt, s) == ClipGame{});

	const ClipGame celeste = clip_game_for(game("Celeste.exe", "Celeste"), s);
	CHECK(celeste.key == "celeste.exe");
	CHECK(!celeste.unsorted());
	CHECK(!celeste.name.empty());

	s.game_rules = {{"celeste.exe", "My Celeste", false}};
	CHECK(clip_game_for(game("CELESTE.EXE", "x"), s) == ClipGame{"celeste.exe", "My Celeste"});

	s.game_rules = {{"celeste.exe", "", true}};
	CHECK(clip_game_for(game("celeste.exe", "Celeste"), s).unsorted());
}

TEST_CASE("plays_save_sound only for filed replays with a sound chosen")
{
	Settings s;
	CHECK(!plays_save_sound(s, ClipKind::replay, true)); // off by default

	s.play_save_sound = true;
	CHECK(!plays_save_sound(s, ClipKind::replay, true)); // no file chosen

	s.save_sound_path = "C:/Sounds/saved.wav";
	CHECK(plays_save_sound(s, ClipKind::replay, true));
	CHECK(!plays_save_sound(s, ClipKind::replay, false));
	CHECK(!plays_save_sound(s, ClipKind::recording, true));
	CHECK(!plays_save_sound(s, ClipKind::screenshot, true));
}
