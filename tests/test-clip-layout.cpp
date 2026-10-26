#include "clip-layout.hpp"

#include <catch2/catch_test_macros.hpp>

#include <set>

using namespace clip_atlas;
namespace fs = std::filesystem;

TEST_CASE("month folders are zero-padded and sort chronologically")
{
	CHECK(month_folder_name({2026, 3}) == "2026-03");
	CHECK(month_folder_name({2026, 10}) == "2026-10");
	CHECK(month_folder_name({2026, 9}) < month_folder_name({2026, 10}));
}

TEST_CASE("year_month_of uses local time")
{
	std::tm tm{};
	tm.tm_year = 2026 - 1900;
	tm.tm_mon = 0; // January
	tm.tm_mday = 15;
	tm.tm_hour = 12;
	tm.tm_isdst = -1;
	auto t = std::chrono::system_clock::from_time_t(std::mktime(&tm));
	YearMonth ym = year_month_of(t);
	CHECK(ym.year == 2026);
	CHECK(ym.month == 1);
}

TEST_CASE("sanitize_folder_name replaces reserved characters")
{
	CHECK(sanitize_folder_name("Half-Life: Alyx") == "Half-Life_ Alyx");
	CHECK(sanitize_folder_name("a<b>c\"d/e\\f|g?h*i") == "a_b_c_d_e_f_g_h_i");
	CHECK(sanitize_folder_name(std::string("tab\there")) == "tab_here");
}

TEST_CASE("sanitize_folder_name trims trailing dots and spaces")
{
	CHECK(sanitize_folder_name("  Celeste. . ") == "Celeste");
	CHECK(sanitize_folder_name("Game...") == "Game");
}

TEST_CASE("sanitize_folder_name avoids Windows device names")
{
	CHECK(sanitize_folder_name("CON") == "CON_");
	CHECK(sanitize_folder_name("nul") == "nul_");
	CHECK(sanitize_folder_name("COM1.exe") == "COM1_.exe");
	CHECK(sanitize_folder_name("CON.txt") == "CON_.txt");
	CHECK(sanitize_folder_name("Console") == "Console");
}

TEST_CASE("sanitize_folder_name is idempotent")
{
	for (const char *name :
	     {"CON", "nul", "COM1.exe", "CON.txt", "Half-Life: Alyx", "  Celeste. . ", "ポケモン", ""}) {
		const std::string once = sanitize_folder_name(name);
		CHECK(sanitize_folder_name(once) == once);
	}
}

TEST_CASE("sanitize_folder_name falls back when nothing usable remains")
{
	CHECK(sanitize_folder_name("") == "Unsorted");
	CHECK(sanitize_folder_name(" . ") == "Unsorted");
	CHECK(sanitize_folder_name("???", "Desktop") == "Desktop");
}

TEST_CASE("sanitize_folder_name keeps UTF-8 intact when truncating")
{
	CHECK(sanitize_folder_name("ポケモン") == "ポケモン");

	std::string long_name(99, 'a');
	long_name += "é"; // 2-byte sequence straddling the 100-byte cap
	std::string out = sanitize_folder_name(long_name);
	CHECK(out == std::string(99, 'a'));
}

TEST_CASE("destination_dir builds <base>/<Game>/<YYYY-MM>")
{
	LayoutOptions opts;
	fs::path base = "clips";
	CHECK(destination_dir(base, "Fortnite", {2026, 10}, opts) == base / "Fortnite" / "2026-10");

	opts.monthly_folders = false;
	CHECK(destination_dir(base, "Fortnite", {2026, 10}, opts) == base / "Fortnite");
}

TEST_CASE("destination_dir routes unknown games to the unsorted folder")
{
	LayoutOptions opts;
	opts.unsorted_folder = "Desktop";
	CHECK(destination_dir("clips", "", {2026, 1}, opts) == fs::path("clips") / "Desktop" / "2026-01");

	opts.unsorted_folder = "";
	CHECK(destination_dir("clips", "", {2026, 1}, opts) == fs::path("clips") / "Unsorted" / "2026-01");
}

TEST_CASE("destination_dir with game folders off is the base itself")
{
	LayoutOptions opts;
	opts.game_folders = false;
	fs::path base = "clips";
	// Monthly folders are on, and still no <YYYY-MM> folder is made.
	CHECK(destination_dir(base, "Fortnite", {2026, 10}, opts) == base);
	CHECK(destination_dir(base, "", {2026, 10}, opts) == base);

	opts.monthly_folders = false;
	CHECK(destination_dir(base, "Fortnite", {2026, 10}, opts) == base);
}

TEST_CASE("unique_destination numbers collisions Windows-style")
{
	std::set<fs::path> taken;
	auto exists = [&](const fs::path &p) {
		return taken.count(p) > 0;
	};
	fs::path dir = "clips";

	CHECK(unique_destination(dir, "Replay.mkv", exists) == dir / "Replay.mkv");

	taken.insert(dir / "Replay.mkv");
	CHECK(unique_destination(dir, "Replay.mkv", exists) == dir / "Replay (2).mkv");

	taken.insert(dir / "Replay (2).mkv");
	CHECK(unique_destination(dir, "Replay.mkv", exists) == dir / "Replay (3).mkv");
}

TEST_CASE("unique_destination gives up rather than looping forever")
{
	auto always = [](const fs::path &) {
		return true;
	};
	CHECK(unique_destination("clips", "Replay.mkv", always).empty());
}
