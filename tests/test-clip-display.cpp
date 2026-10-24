#include "clip-display.hpp"
#include "clip-layout.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <ctime>
#include <string>
#include <vector>

using namespace clip_atlas;
namespace fs = std::filesystem;

namespace {

using Folders = std::vector<std::string>;

// A local point in time from its calendar parts, as mktime() reads them.
std::chrono::system_clock::time_point local_time(int year, int month, int day, int hour, int minute)
{
	std::tm parts{};
	parts.tm_year = year - 1900;
	parts.tm_mon = month - 1;
	parts.tm_mday = day;
	parts.tm_hour = hour;
	parts.tm_min = minute;
	parts.tm_isdst = -1;
	return std::chrono::system_clock::from_time_t(std::mktime(&parts));
}

} // namespace

TEST_CASE("parse_clip_title shortens OBS's default replay, screenshot and recording names", "[clip-display]")
{
	const ClipTitle replay = parse_clip_title("Replay 2026-10-23 21-46-58.mkv");
	CHECK(replay.stamped);
	CHECK(replay.prefix == "Replay");
	CHECK(replay.time == "21:46:58");
	CHECK(replay.suffix.empty());
	CHECK(replay.stem == "Replay 2026-10-23 21-46-58");

	const ClipTitle shot = parse_clip_title("Screenshot 2026-10-21 20-03-11.png");
	CHECK(shot.stamped);
	CHECK(shot.prefix == "Screenshot");
	CHECK(shot.time == "20:03:11");

	// A recording has no prefix: the dock puts the kind's name in front.
	const ClipTitle recording = parse_clip_title("2026-10-20 21-30-00.mp4");
	CHECK(recording.stamped);
	CHECK(recording.prefix.empty());
	CHECK(recording.time == "21:30:00");
}

TEST_CASE("parse_clip_title keeps a custom prefix and the number of a clashing name", "[clip-display]")
{
	const ClipTitle custom = parse_clip_title("Highlight  2026-10-23 21-46-58.mkv");
	CHECK(custom.stamped);
	CHECK(custom.prefix == "Highlight"); // the spaces around it go

	const ClipTitle second = parse_clip_title("Replay 2026-10-23 21-46-58 (2).mkv");
	CHECK(second.stamped);
	CHECK(second.prefix == "Replay");
	CHECK(second.time == "21:46:58");
	CHECK(second.suffix == " (2)");
	CHECK(second.stem == "Replay 2026-10-23 21-46-58 (2)");

	const ClipTitle twelfth = parse_clip_title("2026-10-20 21-30-00 (12).mp4");
	CHECK(twelfth.stamped);
	CHECK(twelfth.suffix == " (12)");
}

TEST_CASE("parse_clip_title shows any other name as typed, without its extension", "[clip-display]")
{
	const ClipTitle renamed = parse_clip_title("Overtime winner.mkv");
	CHECK_FALSE(renamed.stamped);
	CHECK(renamed.stem == "Overtime winner");
	CHECK(renamed.prefix.empty());
	CHECK(renamed.time.empty());

	// Digits that run into a longer word are not OBS's stamp.
	CHECK_FALSE(parse_clip_title("Match2026-10-23 21-46-58.mkv").stamped);
	// Out-of-range parts are not a date and time.
	CHECK_FALSE(parse_clip_title("Replay 2026-13-23 21-46-58.mkv").stamped);
	CHECK_FALSE(parse_clip_title("Replay 2026-10-00 21-46-58.mkv").stamped);
	CHECK_FALSE(parse_clip_title("Replay 2026-10-23 24-00-00.mkv").stamped);
	CHECK_FALSE(parse_clip_title("Replay 2026-10-23 21-60-00.mkv").stamped);
	// Another format OBS can be set to.
	CHECK_FALSE(parse_clip_title("2026-10-23_21-46-58.mkv").stamped);
	// Not a clash number.
	const ClipTitle words = parse_clip_title("Replay 2026-10-23 21-46-58 (best).mkv");
	CHECK_FALSE(words.stamped);
	CHECK(words.stem == "Replay 2026-10-23 21-46-58 (best)");
}

TEST_CASE("parse_clip_title handles names with no extension or extra dots", "[clip-display]")
{
	CHECK(parse_clip_title("no extension").stem == "no extension");
	CHECK(parse_clip_title(".hidden").stem == ".hidden");
	CHECK(parse_clip_title("v1.2 final.mp4").stem == "v1.2 final");
	CHECK(parse_clip_title("").stem.empty());
	CHECK_FALSE(parse_clip_title("").stamped);

	const ClipTitle stamped = parse_clip_title("Replay 2026-10-23 21-46-58");
	CHECK(stamped.stamped);
	CHECK(stamped.time == "21:46:58");
}

TEST_CASE("days_between counts calendar days across months, years and leap days", "[clip-display]")
{
	CHECK(days_between({2026, 10, 23}, {2026, 10, 23}) == 0);
	CHECK(days_between({2026, 10, 22}, {2026, 10, 23}) == 1);
	CHECK(days_between({2026, 10, 23}, {2026, 10, 22}) == -1);
	CHECK(days_between({2026, 9, 30}, {2026, 10, 1}) == 1);
	CHECK(days_between({2025, 12, 31}, {2026, 1, 1}) == 1);
	CHECK(days_between({2024, 2, 28}, {2024, 3, 1}) == 2); // 2024 is a leap year
	CHECK(days_between({2026, 2, 28}, {2026, 3, 1}) == 1);
	CHECK(days_between({2000, 1, 1}, {2026, 10, 23}) == 9792);
	CHECK(days_between({1970, 1, 1}, {1969, 12, 31}) == -1);
}

TEST_CASE("day_group_of: today, yesterday, the previous five days, then months", "[clip-display]")
{
	const LocalDate today{2026, 10, 23};
	CHECK(day_group_of({2026, 10, 23}, today).kind == DayGroupKind::today);
	CHECK(day_group_of({2026, 10, 22}, today).kind == DayGroupKind::yesterday);
	CHECK(day_group_of({2026, 10, 21}, today).kind == DayGroupKind::recent);
	CHECK(day_group_of({2026, 10, 17}, today).kind == DayGroupKind::recent);

	const DayGroup week_old = day_group_of({2026, 10, 16}, today);
	CHECK(week_old.kind == DayGroupKind::month);
	CHECK(week_old.month.year == 2026);
	CHECK(week_old.month.month == 10);

	const DayGroup september = day_group_of({2026, 9, 30}, today);
	CHECK(september.kind == DayGroupKind::month);
	CHECK(september.month.month == 9);
	CHECK(september != week_old);
	CHECK(september == day_group_of({2026, 9, 1}, today));

	// A clip from a later day (the clock was set back) counts as today.
	CHECK(day_group_of({2026, 10, 24}, today).kind == DayGroupKind::today);
	// Groups other than month carry no month.
	CHECK(day_group_of({2026, 10, 22}, today) == DayGroup{DayGroupKind::yesterday, {0, 0}});
}

TEST_CASE("day_group_of follows the calendar across a month and a year boundary", "[clip-display]")
{
	CHECK(day_group_of({2026, 9, 30}, {2026, 10, 1}).kind == DayGroupKind::yesterday);
	CHECK(day_group_of({2025, 12, 28}, {2026, 1, 2}).kind == DayGroupKind::recent);
	const DayGroup december = day_group_of({2025, 12, 20}, {2026, 1, 2});
	CHECK(december.kind == DayGroupKind::month);
	CHECK(december.month.year == 2025);
	CHECK(december.month.month == 12);
}

TEST_CASE("local_date_of gives the local calendar date", "[clip-display]")
{
	CHECK(local_date_of(local_time(2026, 10, 23, 21, 46)) == LocalDate{2026, 10, 23});
	CHECK(local_date_of(local_time(2026, 10, 23, 0, 5)) == LocalDate{2026, 10, 23});
	CHECK(local_date_of(local_time(2026, 12, 31, 23, 59)) == LocalDate{2026, 12, 31});
	CHECK(local_date_of(local_time(2024, 2, 29, 12, 0)) == LocalDate{2024, 2, 29});
}

TEST_CASE("folders_below names the folders between base and a filed clip", "[clip-display]")
{
	const fs::path base = path_from_utf8("clips");
	const Folders monthly = {"Elden Ring", "2026-10"};
	CHECK(folders_below(base, base / "Elden Ring" / "2026-10" / "Replay.mkv") == monthly);
	const Folders flat = {"Elden Ring"};
	CHECK(folders_below(base, base / "Elden Ring" / "Replay.mkv") == flat);
	CHECK(folders_below(base, base / "Replay.mkv").empty());
}

TEST_CASE("folders_below ignores a trailing separator and refuses paths outside base", "[clip-display]")
{
	const fs::path base = path_from_utf8("clips");
	const fs::path with_separator = path_from_utf8("clips/");
	const Folders game = {"Game"};
	CHECK(folders_below(with_separator, base / "Game" / "a.mkv") == game);
	CHECK(folders_below(base, base).empty());
	CHECK(folders_below(base, path_from_utf8("other") / "Game" / "a.mkv").empty());
	CHECK(folders_below(base, path_from_utf8("clips-old") / "Game" / "a.mkv").empty());
	CHECK(folders_below(fs::path(), base / "a.mkv").empty());
}

TEST_CASE("folders_below returns non-ASCII folder names as UTF-8", "[clip-display]")
{
	const fs::path base = path_from_utf8("clips");
	const std::string game = "\xE3\x83\x9D\xE3\x82\xB1\xE3\x83\xA2\xE3\x83\xB3"; // ポケモン
	const Folders expected = {game, "2026-10"};
	CHECK(folders_below(base, base / path_from_utf8(game) / "2026-10" / "a.mkv") == expected);
}
