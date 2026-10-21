// Clip Atlas — which saved clips get sorted, and into which game's folder.
#include "clip-routing.hpp"

namespace clip_atlas {

bool sorting_enabled(const Settings &settings, ClipKind kind)
{
	if (!settings.auto_sort)
		return false;
	switch (kind) {
	case ClipKind::replay:
		return settings.sort_replays;
	case ClipKind::recording:
		return settings.sort_recordings;
	case ClipKind::screenshot:
		return settings.sort_screenshots;
	}
	return false;
}

UserGameRules user_game_rules(const std::vector<GameRuleSetting> &rules)
{
	UserGameRules out;
	for (const GameRuleSetting &rule : rules)
		out.emplace(rule.executable, UserGameRule{rule.display_name, rule.ignore});
	return out;
}

ClipGame clip_game_for(const std::optional<CurrentGame> &game, const Settings &settings)
{
	if (!game)
		return {};
	const ResolvedGame resolved = resolve_game({game->info.executable, game->info.title, ""},
						   user_game_rules(settings.game_rules), BundledGameNames{});
	// An ignored game counts as no game: its clips go to the unsorted folder.
	if (resolved.ignored || resolved.display_name.empty())
		return {};
	return {resolved.folder_key, resolved.display_name};
}

bool plays_save_sound(const Settings &settings, ClipKind kind, bool filed)
{
	return filed && kind == ClipKind::replay && settings.play_save_sound && !settings.save_sound_path.empty();
}

} // namespace clip_atlas
