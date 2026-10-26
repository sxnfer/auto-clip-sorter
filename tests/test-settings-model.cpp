#include "settings-model.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace clip_atlas;

namespace {

// In-memory stand-in for the OBS store. Each value type has its own map, so a key written as
// one type reads back as nullopt through another getter, the same as a mistyped value does.
class FakeStore final : public SettingsReader, public SettingsWriter {
public:
	std::optional<bool> get_bool(const std::string &key) const override { return lookup(bools, key); }
	std::optional<long long> get_int(const std::string &key) const override { return lookup(ints, key); }
	std::optional<std::string> get_string(const std::string &key) const override { return lookup(strings, key); }
	std::optional<std::vector<std::string>> get_string_list(const std::string &key) const override
	{
		return lookup(string_lists, key);
	}
	std::optional<std::vector<SettingsRecord>> get_records(const std::string &key) const override
	{
		return lookup(record_lists, key);
	}

	void set_bool(const std::string &key, bool value) override { bools[key] = value; }
	void set_int(const std::string &key, long long value) override { ints[key] = value; }
	void set_string(const std::string &key, const std::string &value) override { strings[key] = value; }
	void set_string_list(const std::string &key, const std::vector<std::string> &value) override
	{
		string_lists[key] = value;
	}
	void set_records(const std::string &key, const std::vector<SettingsRecord> &value) override
	{
		record_lists[key] = value;
	}

	// True if the key holds a value of any type.
	bool has_key(const std::string &key) const
	{
		return bools.count(key) || ints.count(key) || strings.count(key) || string_lists.count(key) ||
		       record_lists.count(key);
	}

	// Entries across all types, so a stray extra key is noticed.
	std::size_t entry_count() const
	{
		return bools.size() + ints.size() + strings.size() + string_lists.size() + record_lists.size();
	}

	std::map<std::string, bool> bools;
	std::map<std::string, long long> ints;
	std::map<std::string, std::string> strings;
	std::map<std::string, std::vector<std::string>> string_lists;
	std::map<std::string, std::vector<SettingsRecord>> record_lists;

private:
	template<typename Map>
	static std::optional<typename Map::mapped_type> lookup(const Map &map, const std::string &key)
	{
		auto it = map.find(key);
		if (it == map.end())
			return std::nullopt;
		return it->second;
	}
};

// Every key save_settings() must write: the 21 constants in settings_keys.
const char *const kAllKeys[] = {
	settings_keys::kGameFolders,           settings_keys::kPlaySaveSound,  settings_keys::kSaveSoundPath,
	settings_keys::kSaveSoundVolume,       settings_keys::kSchemaVersion,  settings_keys::kClipsFolder,
	settings_keys::kMonthlyFolders,        settings_keys::kUnsortedFolder, settings_keys::kAutoSort,
	settings_keys::kSortReplays,           settings_keys::kSortRecordings, settings_keys::kSortScreenshots,
	settings_keys::kReplayAutoStart,       settings_keys::kReplayAutoStop, settings_keys::kGracePeriodSeconds,
	settings_keys::kSourceSelection,       settings_keys::kChosenSources,  settings_keys::kUseProductName,
	settings_keys::kRecordingDominantGame, settings_keys::kGameRules,      settings_keys::kCheckForUpdates,
};

// Each entry moves one field away from its default and names the key that field is saved
// under. schema_version is not listed: save always writes the current version.
struct FieldChange {
	void (*change)(Settings &);
	const char *key;
};

const FieldChange kFieldChanges[] = {
	{[](Settings &s) { s.clips_folder = "D:/Clips"; }, settings_keys::kClipsFolder},
	{[](Settings &s) { s.game_folders = false; }, settings_keys::kGameFolders},
	{[](Settings &s) { s.monthly_folders = false; }, settings_keys::kMonthlyFolders},
	{[](Settings &s) { s.unsorted_folder = "Misc"; }, settings_keys::kUnsortedFolder},
	{[](Settings &s) { s.auto_sort = false; }, settings_keys::kAutoSort},
	{[](Settings &s) { s.sort_replays = false; }, settings_keys::kSortReplays},
	{[](Settings &s) { s.sort_recordings = false; }, settings_keys::kSortRecordings},
	{[](Settings &s) { s.sort_screenshots = false; }, settings_keys::kSortScreenshots},
	{[](Settings &s) { s.replay_auto_start = false; }, settings_keys::kReplayAutoStart},
	{[](Settings &s) { s.replay_auto_stop = false; }, settings_keys::kReplayAutoStop},
	{[](Settings &s) { s.grace_period_seconds = 42; }, settings_keys::kGracePeriodSeconds},
	{[](Settings &s) { s.play_save_sound = true; }, settings_keys::kPlaySaveSound},
	{[](Settings &s) { s.save_sound_path = "C:/Sounds/saved.wav"; }, settings_keys::kSaveSoundPath},
	{[](Settings &s) { s.save_sound_volume = 35; }, settings_keys::kSaveSoundVolume},
	{[](Settings &s) { s.source_selection = SourceSelection::chosen; }, settings_keys::kSourceSelection},
	{[](Settings &s) { s.chosen_sources = {"uuid-one"}; }, settings_keys::kChosenSources},
	{[](Settings &s) { s.use_product_name = false; }, settings_keys::kUseProductName},
	{[](Settings &s) { s.recording_dominant_game = true; }, settings_keys::kRecordingDominantGame},
	{[](Settings &s) { s.game_rules = {{"celeste.exe", "", true}}; }, settings_keys::kGameRules},
	{[](Settings &s) { s.check_for_updates = false; }, settings_keys::kCheckForUpdates},
};

// Every field moved away from its default, for the round-trip test.
Settings customised_settings()
{
	Settings s;
	s.clips_folder = "D:/Clips/クリップ";
	s.game_folders = false;
	s.monthly_folders = false;
	s.unsorted_folder = "Misc";
	s.auto_sort = false;
	s.sort_replays = false;
	s.sort_recordings = false;
	s.sort_screenshots = false;
	s.replay_auto_start = false;
	s.replay_auto_stop = false;
	s.grace_period_seconds = 42;
	s.play_save_sound = true;
	s.save_sound_path = "C:/Sounds/リプレイ.wav";
	s.save_sound_volume = 35;
	s.source_selection = SourceSelection::chosen;
	s.chosen_sources = {"uuid-one", "uuid-two"};
	s.use_product_name = false;
	s.recording_dominant_game = true;
	s.game_rules = {{"eldenring.exe", "Elden Ring", false}, {"celeste.exe", "", true}};
	s.check_for_updates = false;
	return s;
}

// One game_rules record, built from key/value pairs.
SettingsRecord record_of(std::initializer_list<std::pair<const char *, const char *>> fields)
{
	SettingsRecord record;
	for (const auto &field : fields)
		record[field.first] = field.second;
	return record;
}

// Keys whose stored value differs between two stores, whatever their type.
template<typename Map> void collect_differences(const Map &a, const Map &b, std::set<std::string> &keys)
{
	for (const auto &entry : a) {
		auto it = b.find(entry.first);
		if (it == b.end() || !(it->second == entry.second))
			keys.insert(entry.first);
	}
	for (const auto &entry : b) {
		if (a.find(entry.first) == a.end())
			keys.insert(entry.first);
	}
}

std::set<std::string> differing_keys(const FakeStore &a, const FakeStore &b)
{
	std::set<std::string> keys;
	collect_differences(a.bools, b.bools, keys);
	collect_differences(a.ints, b.ints, keys);
	collect_differences(a.strings, b.strings, keys);
	collect_differences(a.string_lists, b.string_lists, keys);
	collect_differences(a.record_lists, b.record_lists, keys);
	return keys;
}

// The grace period load_settings() returns for a stored integer.
int load_grace(long long stored)
{
	FakeStore store;
	store.ints[settings_keys::kGracePeriodSeconds] = stored;
	return load_settings(store).grace_period_seconds;
}

// The unsorted folder validated() returns for a given value.
std::string validated_unsorted(const std::string &name)
{
	Settings s;
	s.unsorted_folder = name;
	return validated(s).unsorted_folder;
}

} // namespace

TEST_CASE("defaults match the header and docs/PLAN.md")
{
	Settings s;
	CHECK(s.schema_version == Settings::kSchemaVersion);
	CHECK(s.clips_folder.empty()); // empty means OBS's recording path
	CHECK(s.game_folders);
	CHECK(s.monthly_folders);
	CHECK(s.unsorted_folder == "Unsorted");
	CHECK(s.auto_sort);
	CHECK(s.sort_replays);
	CHECK(s.sort_recordings);
	CHECK(s.sort_screenshots);
	CHECK(s.replay_auto_start);
	CHECK(s.replay_auto_stop);
	CHECK(s.grace_period_seconds == 10);
	CHECK(!s.play_save_sound); // off by default (docs/PLAN.md)
	CHECK(s.save_sound_path.empty());
	CHECK(s.save_sound_volume == 80);
	CHECK(s.source_selection == SourceSelection::automatic);
	CHECK(s.chosen_sources.empty());
	CHECK(s.use_product_name);
	CHECK(!s.recording_dominant_game);
	CHECK(s.game_rules.empty());
	CHECK(s.check_for_updates);
	CHECK(validated(s) == s); // the defaults need no correction
}

TEST_CASE("operator== compares every field")
{
	for (const FieldChange &c : kFieldChanges) {
		Settings changed;
		c.change(changed);
		CHECK(changed != Settings{});
	}
	Settings newer_schema;
	newer_schema.schema_version = 2;
	CHECK(newer_schema != Settings{});
	CHECK(Settings{} == Settings{});
}

TEST_CASE("save then load gives back an equal fully customised value")
{
	const Settings original = customised_settings();
	REQUIRE(validated(original) == original); // an exact round trip needs a valid input

	FakeStore store;
	save_settings(original, store);
	const Settings loaded = load_settings(store);
	CHECK(loaded.chosen_sources == original.chosen_sources);
	CHECK(loaded.game_rules == original.game_rules);
	CHECK(loaded == original);
}

TEST_CASE("save then load gives back the defaults")
{
	FakeStore store;
	save_settings(Settings{}, store);
	CHECK(load_settings(store) == Settings{});
}

TEST_CASE("each field is saved under its own key")
{
	FakeStore baseline;
	save_settings(Settings{}, baseline);

	for (const FieldChange &c : kFieldChanges) {
		Settings changed;
		c.change(changed);
		FakeStore store;
		save_settings(changed, store);
		// Exactly one stored key moves, and it is the key for that field.
		CHECK(differing_keys(baseline, store) == std::set<std::string>{c.key});
	}
}

TEST_CASE("save writes every key and the current schema version")
{
	FakeStore store;
	save_settings(Settings{}, store);
	for (const char *key : kAllKeys)
		CHECK(store.has_key(key));
	CHECK(store.entry_count() == std::size(kAllKeys));
	CHECK(store.get_int(settings_keys::kSchemaVersion) == Settings::kSchemaVersion);
}

TEST_CASE("save writes the current schema version even when the struct holds another")
{
	Settings s;
	s.schema_version = 0;
	FakeStore store;
	save_settings(s, store);
	CHECK(store.get_int(settings_keys::kSchemaVersion) == Settings::kSchemaVersion);
}

TEST_CASE("save writes source selection and game rules in their stored form")
{
	Settings s;
	s.source_selection = SourceSelection::chosen;
	s.game_rules = {{"eldenring.exe", "Elden Ring", false}, {"celeste.exe", "", true}};
	FakeStore store;
	save_settings(s, store);

	CHECK(store.get_string(settings_keys::kSourceSelection) == "chosen");
	const std::vector<SettingsRecord> expected = {
		record_of({{"executable", "eldenring.exe"}, {"display_name", "Elden Ring"}, {"ignore", "false"}}),
		record_of({{"executable", "celeste.exe"}, {"display_name", ""}, {"ignore", "true"}}),
	};
	CHECK(store.get_records(settings_keys::kGameRules) == expected);

	s.source_selection = SourceSelection::automatic;
	save_settings(s, store);
	CHECK(store.get_string(settings_keys::kSourceSelection) == "automatic");
}

TEST_CASE("missing keys keep their defaults")
{
	CHECK(load_settings(FakeStore{}) == Settings{});

	FakeStore store;
	store.ints[settings_keys::kGracePeriodSeconds] = 25;
	Settings expected;
	expected.grace_period_seconds = 25;
	CHECK(load_settings(store) == expected);
}

TEST_CASE("mistyped values keep their defaults")
{
	// Each key is stored under a type its getter does not read, so every getter returns nullopt.
	FakeStore store;
	for (const char *key :
	     {settings_keys::kGameFolders, settings_keys::kMonthlyFolders, settings_keys::kAutoSort,
	      settings_keys::kSortReplays, settings_keys::kSortRecordings, settings_keys::kSortScreenshots,
	      settings_keys::kReplayAutoStart, settings_keys::kReplayAutoStop, settings_keys::kUseProductName,
	      settings_keys::kRecordingDominantGame, settings_keys::kCheckForUpdates, settings_keys::kPlaySaveSound})
		store.strings[key] = "false";
	store.strings[settings_keys::kGracePeriodSeconds] = "3";
	store.strings[settings_keys::kSaveSoundVolume] = "3";
	store.bools[settings_keys::kSaveSoundPath] = true;
	store.strings[settings_keys::kSchemaVersion] = "99";
	store.strings[settings_keys::kChosenSources] = "uuid-one";
	store.strings[settings_keys::kGameRules] = "eldenring.exe";
	store.bools[settings_keys::kClipsFolder] = true;
	store.bools[settings_keys::kUnsortedFolder] = true;
	store.bools[settings_keys::kSourceSelection] = true;

	CHECK(load_settings(store) == Settings{});
}

TEST_CASE("grace period is clamped to its range")
{
	CHECK(load_grace(-5) == Settings::kMinGracePeriodSeconds);
	CHECK(load_grace(0) == 0);
	CHECK(load_grace(300) == 300);
	CHECK(load_grace(301) == Settings::kMaxGracePeriodSeconds);
	CHECK(load_grace(9999) == Settings::kMaxGracePeriodSeconds);

	Settings low;
	low.grace_period_seconds = -1;
	CHECK(validated(low).grace_period_seconds == Settings::kMinGracePeriodSeconds);
	Settings high;
	high.grace_period_seconds = 1000;
	CHECK(validated(high).grace_period_seconds == Settings::kMaxGracePeriodSeconds);
}

TEST_CASE("save sound volume is clamped and its path trimmed")
{
	Settings low;
	low.save_sound_volume = -1;
	CHECK(validated(low).save_sound_volume == Settings::kMinSaveSoundVolume);
	Settings high;
	high.save_sound_volume = 101;
	CHECK(validated(high).save_sound_volume == Settings::kMaxSaveSoundVolume);
	Settings edge;
	edge.save_sound_volume = 0;
	CHECK(validated(edge).save_sound_volume == 0);

	FakeStore store;
	store.ints[settings_keys::kSaveSoundVolume] = 1LL << 40;
	store.strings[settings_keys::kSaveSoundPath] = "  D:/Sounds/saved.wav \t";
	const Settings loaded = load_settings(store);
	CHECK(loaded.save_sound_volume == Settings::kMaxSaveSoundVolume);
	CHECK(loaded.save_sound_path == "D:/Sounds/saved.wav");
}

TEST_CASE("huge stored grace values do not overflow")
{
	constexpr long long kHuge = std::numeric_limits<long long>::max();
	constexpr long long kHugeNegative = std::numeric_limits<long long>::min();
	// A plain cast to int would wrap these. (1LL << 32) + 10 and -(1LL << 32) + 10 both wrap to 10,
	// a believable grace period that would hide the bug.
	CHECK(load_grace(kHuge) == Settings::kMaxGracePeriodSeconds);
	CHECK(load_grace(kHugeNegative) == Settings::kMinGracePeriodSeconds);
	CHECK(load_grace((1LL << 32) + 10) == Settings::kMaxGracePeriodSeconds);
	CHECK(load_grace(-(1LL << 32) + 10) == Settings::kMinGracePeriodSeconds);
	CHECK(load_grace(std::numeric_limits<int>::max()) == Settings::kMaxGracePeriodSeconds);
	CHECK(load_grace(std::numeric_limits<int>::min()) == Settings::kMinGracePeriodSeconds);
}

TEST_CASE("unsorted folder is sanitised like a folder name")
{
	CHECK(validated_unsorted("CON") == "CON_");
	CHECK(validated_unsorted("a:b") == "a_b");
	CHECK(validated_unsorted("") == "Unsorted");
	CHECK(validated_unsorted("   ") == "Unsorted");
	CHECK(validated_unsorted("Desktop") == "Desktop");
}

TEST_CASE("clips folder is trimmed and an empty value stays empty")
{
	Settings s;
	s.clips_folder = "  D:/Clips \t\n";
	CHECK(validated(s).clips_folder == "D:/Clips");
	s.clips_folder = " \t ";
	CHECK(validated(s).clips_folder.empty());
	s.clips_folder = "My Clips"; // inner spaces are part of the name
	CHECK(validated(s).clips_folder == "My Clips");
	s.clips_folder = "  D:/クリップ  ";
	CHECK(validated(s).clips_folder == "D:/クリップ");
}

TEST_CASE("chosen sources are trimmed, blanks and repeats dropped, order kept")
{
	Settings s;
	s.chosen_sources = {" uuid-b ", "uuid-a", "", "uuid-b", "   ", "uuid-a ", "uuid-c"};
	const std::vector<std::string> expected = {"uuid-b", "uuid-a", "uuid-c"};
	CHECK(validated(s).chosen_sources == expected);
}

TEST_CASE("game rules are lowercased and trimmed, empty ones dropped, first duplicate wins")
{
	Settings s;
	s.game_rules = {
		{"  EldenRing.EXE  ", "  Elden Ring ", false}, // kept, normalised
		{"", "Nameless", true},                        // no executable
		{"   ", "Blank", false},                       // whitespace only
		{"celeste.exe", "", true},                     // no display name is fine
		{"ELDENRING.EXE", "Second", true},             // repeat of the first rule
		{"Hades.exe", "Hades", false},
	};
	const std::vector<GameRuleSetting> expected = {
		{"eldenring.exe", "Elden Ring", false},
		{"celeste.exe", "", true},
		{"hades.exe", "Hades", false},
	};
	CHECK(validated(s).game_rules == expected);
}

TEST_CASE("game rule records load with missing keys as empty or false")
{
	FakeStore store;
	store.record_lists[settings_keys::kGameRules] = {
		record_of({{"executable", "Foo.exe"}}),
		record_of({{"executable", "bar.exe"}, {"display_name", "Bar"}, {"ignore", "true"}}),
		record_of({{"display_name", "No executable"}, {"ignore", "true"}}),
	};
	const std::vector<GameRuleSetting> expected = {
		{"foo.exe", "", false},
		{"bar.exe", "Bar", true},
	};
	CHECK(load_settings(store).game_rules == expected);
}

TEST_CASE("source selection reads its strings and falls back to automatic")
{
	FakeStore store;
	store.strings[settings_keys::kSourceSelection] = "chosen";
	CHECK(load_settings(store).source_selection == SourceSelection::chosen);

	store.strings[settings_keys::kSourceSelection] = "sometimes";
	CHECK(load_settings(store).source_selection == SourceSelection::automatic);

	store.strings[settings_keys::kSourceSelection] = "";
	CHECK(load_settings(store).source_selection == SourceSelection::automatic);

	FakeStore mistyped;
	mistyped.bools[settings_keys::kSourceSelection] = true;
	CHECK(load_settings(mistyped).source_selection == SourceSelection::automatic);
}

TEST_CASE("load returns validated values")
{
	FakeStore store;
	store.strings[settings_keys::kClipsFolder] = "  D:/Clips  ";
	store.strings[settings_keys::kUnsortedFolder] = "a:b";
	store.ints[settings_keys::kGracePeriodSeconds] = 999;
	store.string_lists[settings_keys::kChosenSources] = {" uuid ", "", "uuid"};

	const Settings loaded = load_settings(store);
	CHECK(loaded.clips_folder == "D:/Clips");
	CHECK(loaded.unsorted_folder == "a_b");
	CHECK(loaded.grace_period_seconds == Settings::kMaxGracePeriodSeconds);
	CHECK(loaded.chosen_sources == std::vector<std::string>{"uuid"});
}

TEST_CASE("an older schema version goes through migration without error")
{
	FakeStore store;
	store.ints[settings_keys::kSchemaVersion] = 0;
	store.strings[settings_keys::kClipsFolder] = "D:/Clips";
	store.ints[settings_keys::kGracePeriodSeconds] = 25;

	Settings expected;
	expected.clips_folder = "D:/Clips";
	expected.grace_period_seconds = 25;
	CHECK(load_settings(store) == expected);

	// An absurd negative version must not throw either.
	store.ints[settings_keys::kSchemaVersion] = std::numeric_limits<long long>::min();
	CHECK(load_settings(store).schema_version == Settings::kSchemaVersion);
}

TEST_CASE("migrate_settings does not change settings for an older version")
{
	Settings s = customised_settings();
	const Settings before = s;
	FakeStore store;
	migrate_settings(s, 0, store);
	CHECK(s == before);
}

TEST_CASE("a newer schema version loads the keys this build knows")
{
	FakeStore store;
	store.ints[settings_keys::kSchemaVersion] = 99;
	store.ints[settings_keys::kGracePeriodSeconds] = 42;
	store.strings[settings_keys::kClipsFolder] = "D:/Clips";
	store.bools["future_toggle"] = true; // keys this build does not know are ignored
	store.string_lists["future_list"] = {"x"};

	const Settings loaded = load_settings(store);
	CHECK(loaded.grace_period_seconds == 42);
	CHECK(loaded.clips_folder == "D:/Clips");
	CHECK(loaded.schema_version == Settings::kSchemaVersion); // written back as this build's version
}

TEST_CASE("validated is idempotent")
{
	Settings messy;
	messy.schema_version = 0;
	messy.clips_folder = "  D:/Clips \t";
	messy.unsorted_folder = "a:b";
	messy.grace_period_seconds = 9000;
	messy.chosen_sources = {" a ", "a", "", "b "};
	messy.game_rules = {{" Foo.EXE ", " Foo ", true}, {"foo.exe", "Dup", false}, {"", "", false}};

	const Settings once = validated(messy);
	CHECK(once.schema_version == Settings::kSchemaVersion);
	CHECK(validated(once) == once);
	CHECK(validated(validated(messy)) == once);

	for (const char *name : {"CON", "COM1.exe", "CON.txt", "a:b", "", "  x. ", "???", "Unsorted"}) {
		Settings s;
		s.unsorted_folder = name;
		const Settings v = validated(s);
		CHECK(validated(v) == v);
	}
}
