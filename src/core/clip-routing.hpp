// Clip Atlas — which saved clips get sorted, and into which game's folder.
//
// Replays, recordings and screenshots all answer the same two questions before
// a file moves: is sorting this kind of clip switched on, and which game does
// it belong to? The answers live here so the three OBS event handlers agree.
//
// Pure C++17 with no OBS dependency. All strings are UTF-8.
#pragma once

#include "game-state.hpp"
#include "name-resolver.hpp"
#include "settings-model.hpp"

#include <optional>
#include <string>
#include <vector>

namespace clip_atlas {

enum class ClipKind {
	replay,
	recording,
	screenshot,
};

// Settings::auto_sort (the master switch) and the kind's own toggle
// (sort_replays, sort_recordings or sort_screenshots).
bool sorting_enabled(const Settings &settings, ClipKind kind);

// The game folder a clip goes in. Both fields empty means the unsorted folder.
struct ClipGame {
	std::string key;  // ResolvedGame::folder_key (the lowercased exe)
	std::string name; // ResolvedGame::display_name, used as the folder name

	bool unsorted() const { return name.empty(); }
	bool operator==(const ClipGame &o) const { return key == o.key && name == o.name; }
	bool operator!=(const ClipGame &o) const { return !(*this == o); }
};

// Settings::game_rules as the resolver's map. validated() has already
// lowercased the executables and removed duplicates; for a duplicate that is
// still present, the first rule wins.
UserGameRules user_game_rules(const std::vector<GameRuleSetting> &rules);

// Resolves `game` (from GameState::current() or game_for_save()) with
// resolve_game({executable, title, ""}, user_game_rules(settings.game_rules),
// BundledGameNames{}). nullopt, an ignored game, or an empty display name
// give the unsorted ClipGame{}.
ClipGame clip_game_for(const std::optional<CurrentGame> &game, const Settings &settings);

// Whether a clip that has just been filed should play the save sound:
// play_save_sound is on, save_sound_path is not empty, the clip is a replay
// (the sound confirms a replay hotkey press; recordings and screenshots stay
// silent), and the file was moved or copied into its folder (`filed`).
bool plays_save_sound(const Settings &settings, ClipKind kind, bool filed);

} // namespace clip_atlas
