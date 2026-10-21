// Clip Atlas — turns what a capture source tells us about a hooked window into
// a game identity: a stable folder key and a friendly display name.
//
// The key is the lowercased executable ("eldenring.exe") so a game whose
// window title changes between sessions is still recognised as one game. The
// display name is what users see and what the clip folder is called.
//
// Pure C++17 with no OBS dependency. All strings are UTF-8.
#pragma once

#include <map>
#include <string>

namespace clip_atlas {

// What the hooked/unhooked calldata and (later) process-info give us.
struct GameIdentityInput {
	// File name of the process image, e.g. "EldenRing.exe". OBS reports
	// "unknown" when it cannot open the process; treat that like empty.
	std::string executable;
	std::string window_title;
	// VERSIONINFO ProductName of the executable. Optional; empty if unknown.
	std::string product_name;
};

// A user rule from the Games settings page.
struct UserGameRule {
	std::string display_name; // empty: keep resolving, only `ignore` applies
	bool ignore = false;      // never treat this executable as a game
};

// Keys of both maps are lowercased executables including ".exe".
using UserGameRules = std::map<std::string, UserGameRule>;
using BundledGameNames = std::map<std::string, std::string>;

enum class NameSource {
	none,         // nothing usable; the clip goes to the unsorted folder
	user_rule,    // UserGameRule::display_name
	bundled,      // games.json
	product_name, // executable's ProductName
	window_title, // cleaned window title
	executable,   // executable name without ".exe"
};

struct ResolvedGame {
	// Lowercased executable ("eldenring.exe"). Empty only if no executable
	// was reported, in which case `display_name` came from the title alone.
	std::string folder_key;
	// Friendly name; also the clip folder name (sanitised later by
	// clip-layout). Empty when `source` is NameSource::none.
	std::string display_name;
	NameSource source = NameSource::none;
	// A user rule says to ignore this executable. `display_name` is still
	// filled in so the UI can show what was ignored.
	bool ignored = false;
};

// Resolves in this order, first non-empty result wins:
//   1. user rule display name (matched on the lowercased executable)
//   2. bundled name
//   3. product name, unless is_generic_product_name()
//   4. clean_window_title(window_title)
//   5. executable_stem(executable)
// `ignored` comes from the user rule regardless of which step named the game.
// Matching on the executable is ASCII case-insensitive.
ResolvedGame resolve_game(const GameIdentityInput &input, const UserGameRules &rules, const BundledGameNames &bundled);

// "EldenRing.EXE" -> "eldenring.exe". "unknown" and blank input give "".
// Only ASCII letters are lowercased; other bytes pass through unchanged.
std::string normalize_executable(const std::string &executable);

// "EldenRing.exe" -> "EldenRing". Strips one trailing ".exe" in any case.
// "unknown" and blank input give "".
std::string executable_stem(const std::string &executable);

// True for engine/runtime names that say nothing about the game, e.g.
// "Unreal Engine", "Unity", "Java(TM) Platform SE binary". Compared after
// trimming, ASCII case-insensitively, against a fixed list.
bool is_generic_product_name(const std::string &product_name);

// Removes noise that changes between sessions or launchers:
//   - version tokens such as "1.21.4", "v1.2", "(1.0.3)"
//   - trailing launcher suffixes such as " - Steam" or " | Epic Games"
//   - surrounding whitespace and separators left behind (" - ", ":", "|")
// "Minecraft 1.21.4" -> "Minecraft"; "Celeste - Steam" -> "Celeste".
// Returns "" if nothing meaningful is left.
std::string clean_window_title(const std::string &title);

} // namespace clip_atlas
