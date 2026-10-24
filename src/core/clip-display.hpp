// Clip Atlas — how the dock titles clips, groups them by day, and says where one was filed.
//
// Titles: OBS names a clip "<prefix> <YYYY-MM-DD HH-MM-SS>.<ext>" by default: "Replay 2026-10-23 21-46-58.mkv",
// "Screenshot 2026-10-23 21-12-40.png", and "2026-10-23 19-40-03.mp4" for a recording, which has no prefix. The dock
// lists clips under a header for their day, so it shows such a name as its prefix and time ("Replay 21:46:58"). The
// number clip-layout adds to a clashing name (" (2)") is kept. Any other name is shown as it is, without its extension.
//
// Groups, in local time: today, yesterday, the five days before that, then one group per calendar month.
//
// Pure C++17 with no OBS dependency. Strings are UTF-8.
#pragma once

#include "clip-layout.hpp"

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace clip_atlas {

struct ClipTitle {
	// True for OBS's default name: the stem ends with "YYYY-MM-DD HH-MM-SS", then optionally " (N)", and the text
	// before the stamp is empty or ends in a space.
	bool stamped = false;
	// The text before the stamp, without the spaces around it ("Replay"); "" for OBS's recordings. Stamped only.
	std::string prefix;
	// "21:46:58". Stamped only.
	std::string time;
	// " (2)" when clip-layout numbered the file, otherwise "". Stamped only.
	std::string suffix;
	// The file name without its extension, or the whole name when it has none. Always set.
	std::string stem;
};

// Reads a clip's file name (no folders) as the header describes. No I/O.
ClipTitle parse_clip_title(const std::string &file_name);

// A day on the local calendar.
struct LocalDate {
	int year = 1970;
	int month = 1; // 1-12
	int day = 1;   // 1-31

	bool operator==(const LocalDate &o) const { return year == o.year && month == o.month && day == o.day; }
	bool operator!=(const LocalDate &o) const { return !(*this == o); }
};

// The local date of a point in time.
LocalDate local_date_of(std::chrono::system_clock::time_point t);

// Calendar days from `from` to `to`: 1 when `to` is the day after `from`, negative when it comes before.
long long days_between(const LocalDate &from, const LocalDate &to);

enum class DayGroupKind {
	today,     // today, or a later day (the clock was changed)
	yesterday, // the day before today
	recent,    // 2 to 6 days before today
	month,     // 7 or more days before today: the clip's own calendar month
};

struct DayGroup {
	DayGroupKind kind = DayGroupKind::today;
	// The clip's year and month for DayGroupKind::month; {0, 0} for the others.
	YearMonth month{0, 0};

	bool operator==(const DayGroup &o) const
	{
		return kind == o.kind && month.year == o.month.year && month.month == o.month.month;
	}
	bool operator!=(const DayGroup &o) const { return !(*this == o); }
};

// The group a clip saved on `clip_day` belongs to when today is `today`.
DayGroup day_group_of(const LocalDate &clip_day, const LocalDate &today);

// The folders between `base` and `file`, as UTF-8: {"Elden Ring", "2026-10"} for <base>/Elden Ring/2026-10/clip.mkv.
// Empty for a file directly in `base`, for `base` itself, and for a file outside it. Paths are compared component by
// component after lexically_normal(), as game_folder_of() does, so a trailing separator on `base` makes no
// difference. No I/O.
std::vector<std::string> folders_below(const std::filesystem::path &base, const std::filesystem::path &file);

} // namespace clip_atlas
