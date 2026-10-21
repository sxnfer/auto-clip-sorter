#include "settings-model.hpp"

#include "clip-layout.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <utility>

namespace clip_atlas {

namespace {

// Stored values of source_selection.
constexpr const char *kSourceAutomatic = "automatic";
constexpr const char *kSourceChosen = "chosen";

// Keys inside each game_rules record.
constexpr const char *kRuleExecutable = "executable";
constexpr const char *kRuleDisplayName = "display_name";
constexpr const char *kRuleIgnore = "ignore";

bool is_ascii_space(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

// Only ASCII whitespace is removed. ASCII bytes never appear inside a multi-byte UTF-8
// sequence, so trimming cannot split a character.
std::string trim_ascii(const std::string &s)
{
	std::string::size_type begin = 0;
	while (begin < s.size() && is_ascii_space(s[begin]))
		++begin;
	std::string::size_type end = s.size();
	while (end > begin && is_ascii_space(s[end - 1]))
		--end;
	return s.substr(begin, end - begin);
}

// Done by hand rather than with std::tolower, so the result never depends on the C locale.
std::string lower_ascii(std::string s)
{
	for (char &c : s) {
		if (c >= 'A' && c <= 'Z')
			c = static_cast<char>(c - 'A' + 'a');
	}
	return s;
}

// The reader returns a long long but the fields are int. Clamping first keeps a huge stored
// value from wrapping into a plausible one; validated() then applies the real range.
int clamp_to_int(long long value)
{
	constexpr long long kLowest = std::numeric_limits<int>::min();
	constexpr long long kHighest = std::numeric_limits<int>::max();
	return static_cast<int>(std::clamp(value, kLowest, kHighest));
}

// A missing or mistyped key comes back as nullopt and leaves the field at its default, so
// one bad value cannot reset the others.
void read_bool(const SettingsReader &reader, const char *key, bool &field)
{
	if (auto value = reader.get_bool(key))
		field = *value;
}

void read_int(const SettingsReader &reader, const char *key, int &field)
{
	if (auto value = reader.get_int(key))
		field = clamp_to_int(*value);
}

void read_string(const SettingsReader &reader, const char *key, std::string &field)
{
	if (auto value = reader.get_string(key))
		field = std::move(*value);
}

void read_string_list(const SettingsReader &reader, const char *key, std::vector<std::string> &field)
{
	if (auto value = reader.get_string_list(key))
		field = std::move(*value);
}

// An unrecognised string leaves the field alone, so it keeps the default (automatic).
void read_source_selection(const SettingsReader &reader, SourceSelection &field)
{
	auto value = reader.get_string(settings_keys::kSourceSelection);
	if (!value)
		return;
	if (*value == kSourceAutomatic)
		field = SourceSelection::automatic;
	else if (*value == kSourceChosen)
		field = SourceSelection::chosen;
}

std::string record_value(const SettingsRecord &record, const char *key)
{
	auto it = record.find(key);
	return it == record.end() ? std::string() : it->second;
}

void read_game_rules(const SettingsReader &reader, std::vector<GameRuleSetting> &field)
{
	auto records = reader.get_records(settings_keys::kGameRules);
	if (!records)
		return;
	field.clear();
	for (const SettingsRecord &record : *records) {
		GameRuleSetting rule;
		rule.executable = record_value(record, kRuleExecutable);
		rule.display_name = record_value(record, kRuleDisplayName);
		rule.ignore = record_value(record, kRuleIgnore) == "true";
		field.push_back(std::move(rule));
	}
}

std::vector<std::string> validated_sources(const std::vector<std::string> &sources)
{
	std::vector<std::string> out;
	std::set<std::string> seen;
	for (const std::string &source : sources) {
		std::string trimmed = trim_ascii(source);
		// The first occurrence wins, so the order the user chose is kept.
		if (!trimmed.empty() && seen.insert(trimmed).second)
			out.push_back(std::move(trimmed));
	}
	return out;
}

std::vector<GameRuleSetting> validated_rules(const std::vector<GameRuleSetting> &rules)
{
	std::vector<GameRuleSetting> out;
	std::set<std::string> seen;
	for (const GameRuleSetting &rule : rules) {
		GameRuleSetting clean;
		// Executable matching is case-insensitive, so one spelling is stored.
		clean.executable = lower_ascii(trim_ascii(rule.executable));
		clean.display_name = trim_ascii(rule.display_name);
		clean.ignore = rule.ignore;
		// A rule with no executable matches nothing. For a repeated executable the first rule
		// wins, so a later duplicate cannot quietly change what the user set.
		if (clean.executable.empty() || !seen.insert(clean.executable).second)
			continue;
		out.push_back(std::move(clean));
	}
	return out;
}

std::vector<SettingsRecord> rules_to_records(const std::vector<GameRuleSetting> &rules)
{
	std::vector<SettingsRecord> records;
	records.reserve(rules.size());
	for (const GameRuleSetting &rule : rules) {
		SettingsRecord record;
		record[kRuleExecutable] = rule.executable;
		record[kRuleDisplayName] = rule.display_name;
		record[kRuleIgnore] = rule.ignore ? "true" : "false";
		records.push_back(std::move(record));
	}
	return records;
}

} // namespace

bool Settings::operator==(const Settings &o) const
{
	return schema_version == o.schema_version && clips_folder == o.clips_folder &&
	       monthly_folders == o.monthly_folders && unsorted_folder == o.unsorted_folder &&
	       auto_sort == o.auto_sort && sort_replays == o.sort_replays && sort_recordings == o.sort_recordings &&
	       sort_screenshots == o.sort_screenshots && replay_auto_start == o.replay_auto_start &&
	       replay_auto_stop == o.replay_auto_stop && grace_period_seconds == o.grace_period_seconds &&
	       play_save_sound == o.play_save_sound && save_sound_path == o.save_sound_path &&
	       save_sound_volume == o.save_sound_volume && source_selection == o.source_selection &&
	       chosen_sources == o.chosen_sources && use_product_name == o.use_product_name &&
	       recording_dominant_game == o.recording_dominant_game && game_rules == o.game_rules &&
	       check_for_updates == o.check_for_updates;
}

Settings validated(Settings settings)
{
	settings.schema_version = Settings::kSchemaVersion;
	settings.clips_folder = trim_ascii(settings.clips_folder);
	// "Unsorted" is the fallback when nothing usable is left, such as an empty name.
	settings.unsorted_folder = sanitize_folder_name(settings.unsorted_folder, "Unsorted");
	settings.grace_period_seconds = std::clamp(settings.grace_period_seconds, Settings::kMinGracePeriodSeconds,
						   Settings::kMaxGracePeriodSeconds);
	settings.save_sound_path = trim_ascii(settings.save_sound_path);
	settings.save_sound_volume =
		std::clamp(settings.save_sound_volume, Settings::kMinSaveSoundVolume, Settings::kMaxSaveSoundVolume);
	settings.chosen_sources = validated_sources(settings.chosen_sources);
	settings.game_rules = validated_rules(settings.game_rules);
	return settings;
}

Settings load_settings(const SettingsReader &reader)
{
	// Starts from the defaults, so every key the reader cannot supply keeps its default.
	Settings settings;
	read_int(reader, settings_keys::kSchemaVersion, settings.schema_version);
	read_string(reader, settings_keys::kClipsFolder, settings.clips_folder);
	read_bool(reader, settings_keys::kMonthlyFolders, settings.monthly_folders);
	read_string(reader, settings_keys::kUnsortedFolder, settings.unsorted_folder);
	read_bool(reader, settings_keys::kAutoSort, settings.auto_sort);
	read_bool(reader, settings_keys::kSortReplays, settings.sort_replays);
	read_bool(reader, settings_keys::kSortRecordings, settings.sort_recordings);
	read_bool(reader, settings_keys::kSortScreenshots, settings.sort_screenshots);
	read_bool(reader, settings_keys::kReplayAutoStart, settings.replay_auto_start);
	read_bool(reader, settings_keys::kReplayAutoStop, settings.replay_auto_stop);
	read_int(reader, settings_keys::kGracePeriodSeconds, settings.grace_period_seconds);
	read_bool(reader, settings_keys::kPlaySaveSound, settings.play_save_sound);
	read_string(reader, settings_keys::kSaveSoundPath, settings.save_sound_path);
	read_int(reader, settings_keys::kSaveSoundVolume, settings.save_sound_volume);
	read_source_selection(reader, settings.source_selection);
	read_string_list(reader, settings_keys::kChosenSources, settings.chosen_sources);
	read_bool(reader, settings_keys::kUseProductName, settings.use_product_name);
	read_bool(reader, settings_keys::kRecordingDominantGame, settings.recording_dominant_game);
	read_game_rules(reader, settings.game_rules);
	read_bool(reader, settings_keys::kCheckForUpdates, settings.check_for_updates);

	// A missing or mistyped schema_version reads as the current one, so nothing is migrated.
	// A newer one is left as it is: the keys this build knows were read above, the rest are ignored.
	if (settings.schema_version < Settings::kSchemaVersion)
		migrate_settings(settings, settings.schema_version, reader);
	return validated(std::move(settings));
}

void save_settings(const Settings &settings, SettingsWriter &writer)
{
	// Always the current version, because this build writes the format it understands.
	writer.set_int(settings_keys::kSchemaVersion, Settings::kSchemaVersion);
	writer.set_string(settings_keys::kClipsFolder, settings.clips_folder);
	writer.set_bool(settings_keys::kMonthlyFolders, settings.monthly_folders);
	writer.set_string(settings_keys::kUnsortedFolder, settings.unsorted_folder);
	writer.set_bool(settings_keys::kAutoSort, settings.auto_sort);
	writer.set_bool(settings_keys::kSortReplays, settings.sort_replays);
	writer.set_bool(settings_keys::kSortRecordings, settings.sort_recordings);
	writer.set_bool(settings_keys::kSortScreenshots, settings.sort_screenshots);
	writer.set_bool(settings_keys::kReplayAutoStart, settings.replay_auto_start);
	writer.set_bool(settings_keys::kReplayAutoStop, settings.replay_auto_stop);
	writer.set_int(settings_keys::kGracePeriodSeconds, settings.grace_period_seconds);
	writer.set_bool(settings_keys::kPlaySaveSound, settings.play_save_sound);
	writer.set_string(settings_keys::kSaveSoundPath, settings.save_sound_path);
	writer.set_int(settings_keys::kSaveSoundVolume, settings.save_sound_volume);
	writer.set_string(settings_keys::kSourceSelection,
			  settings.source_selection == SourceSelection::chosen ? kSourceChosen : kSourceAutomatic);
	writer.set_string_list(settings_keys::kChosenSources, settings.chosen_sources);
	writer.set_bool(settings_keys::kUseProductName, settings.use_product_name);
	writer.set_bool(settings_keys::kRecordingDominantGame, settings.recording_dominant_game);
	writer.set_records(settings_keys::kGameRules, rules_to_records(settings.game_rules));
	writer.set_bool(settings_keys::kCheckForUpdates, settings.check_for_updates);
}

void migrate_settings([[maybe_unused]] Settings &settings, [[maybe_unused]] int from_version,
		      [[maybe_unused]] const SettingsReader &reader)
{
	// Nothing to upgrade yet, since version 1 is the first schema. A later schema adds one step
	// here per version, e.g. `if (from_version < 2) { ... }`, in ascending order, so a file several
	// versions old runs each step in turn. A step reads keys that were removed through `reader`
	// and writes the new shape into `settings`. It must not throw: it runs on whatever a user's
	// file contains.
}

} // namespace clip_atlas
