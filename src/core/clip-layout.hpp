// Clip Atlas — folder layout for sorted clips.
//
// Layout:  <base>/<Game>/<YYYY-MM>/<original file name>
//          <base>/<Game>/<original file name>          (monthly folders off)
//          <base>/<original file name>                 (game folders off)
//
// Pure C++17 with no OBS dependency so it can be unit-tested in isolation.
// All strings are UTF-8 (as OBS hands them to us).
#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <string>

namespace clip_atlas {

struct LayoutOptions {
	// Group clips into a folder per game. Off: every clip goes straight into
	// <base>, and monthly_folders and unsorted_folder have no effect.
	bool game_folders = true;
	// Group clips into <YYYY-MM> subfolders inside each game folder.
	bool monthly_folders = true;
	// Folder used when no game could be detected (e.g. desktop capture).
	std::string unsorted_folder = "Unsorted";
};

struct YearMonth {
	int year;
	int month; // 1-12
};

// Local-time year/month of a point in time.
YearMonth year_month_of(std::chrono::system_clock::time_point t);

// "2026-10"; zero-padded so folders sort chronologically.
std::string month_folder_name(YearMonth ym);

// Makes a UTF-8 name safe as a single Windows path component: replaces
// reserved and control characters, trims trailing dots/spaces, avoids device
// names (CON, NUL, COM1, ...) and caps the length without splitting a UTF-8
// sequence. Returns `fallback` if nothing usable is left.
std::string sanitize_folder_name(const std::string &name, const std::string &fallback = "Unsorted");

// Converts a UTF-8 string to a path without going through the ANSI code page.
std::filesystem::path path_from_utf8(const std::string &utf8);

// Directory a clip belongs in. An empty `game` routes to the unsorted folder.
// With game folders off it is always `base`.
std::filesystem::path destination_dir(const std::filesystem::path &base, const std::string &game, YearMonth when,
				      const LayoutOptions &options);

using ExistsFn = std::function<bool(const std::filesystem::path &)>;

// `dir / file_name`, or the first free "name (2).ext", "name (3).ext", ...
// `exists` is injectable for tests; defaults to std::filesystem::exists.
std::filesystem::path unique_destination(const std::filesystem::path &dir, const std::filesystem::path &file_name,
					 const ExistsFn &exists = {});

} // namespace clip_atlas
