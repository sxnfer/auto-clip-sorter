// Clip Atlas — every user setting, its default, and how it is validated.
//
// Settings are read and written through a small key/value interface so this
// file stays free of OBS; obs/settings-store adapts it to obs_data_t JSON.
// Later milestones add pages to the settings dialog but should only need to
// read fields that already exist here.
//
// Pure C++17 with no OBS dependency. All strings are UTF-8.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace clip_atlas {

enum class SourceSelection {
	automatic, // every game_capture and window_capture source
	chosen,    // only the sources listed in Settings::chosen_sources
};

struct GameRuleSetting {
	std::string executable;   // stored lowercased, e.g. "eldenring.exe"
	std::string display_name; // empty: only `ignore` applies
	bool ignore = false;

	bool operator==(const GameRuleSetting &o) const
	{
		return executable == o.executable && display_name == o.display_name && ignore == o.ignore;
	}
};

struct Settings {
	static constexpr int kSchemaVersion = 1;
	static constexpr int kMinGracePeriodSeconds = 0;
	static constexpr int kMaxGracePeriodSeconds = 300;
	static constexpr int kMinSaveSoundVolume = 0;
	static constexpr int kMaxSaveSoundVolume = 100;

	int schema_version = kSchemaVersion;

	// General
	std::string clips_folder;    // empty: OBS's current recording path
	bool game_folders = true;    // <Game>/ subfolders; off: clips stay directly in clips_folder
	bool monthly_folders = true; // <Game>/<YYYY-MM>/ subfolders
	std::string unsorted_folder = "Unsorted";
	bool auto_sort = true; // master switch (dock header toggle)
	bool sort_replays = true;
	bool sort_recordings = true;  // used from M2
	bool sort_screenshots = true; // used from M2

	// Replay Buffer
	bool replay_auto_start = true;
	bool replay_auto_stop = true;
	int grace_period_seconds = 10; // also how long a closed game still gets saves

	// Save sound (M2): played through Windows once a replay has been sorted
	bool play_save_sound = false;
	std::string save_sound_path; // UTF-8 path of a .wav; empty: no sound (no default ships yet)
	int save_sound_volume = 80;  // percent, [kMinSaveSoundVolume, kMaxSaveSoundVolume]

	// Detection
	SourceSelection source_selection = SourceSelection::automatic;
	std::vector<std::string> chosen_sources; // source UUIDs
	bool use_product_name = true;            // used from M4
	bool recording_dominant_game = false;    // used from M2

	// Games
	std::vector<GameRuleSetting> game_rules;

	// Advanced
	bool check_for_updates = true; // used from M5

	bool operator==(const Settings &o) const;
	bool operator!=(const Settings &o) const { return !(*this == o); }
};

// A flat record, used for list-of-object settings such as game rules.
using SettingsRecord = std::map<std::string, std::string>;

// Read side of the storage. Every getter returns nullopt when the key is
// missing or holds a value of another type; load_settings() then keeps the
// default for that field.
class SettingsReader {
public:
	virtual ~SettingsReader() = default;
	virtual std::optional<bool> get_bool(const std::string &key) const = 0;
	virtual std::optional<long long> get_int(const std::string &key) const = 0;
	virtual std::optional<std::string> get_string(const std::string &key) const = 0;
	virtual std::optional<std::vector<std::string>> get_string_list(const std::string &key) const = 0;
	virtual std::optional<std::vector<SettingsRecord>> get_records(const std::string &key) const = 0;
};

class SettingsWriter {
public:
	virtual ~SettingsWriter() = default;
	virtual void set_bool(const std::string &key, bool value) = 0;
	virtual void set_int(const std::string &key, long long value) = 0;
	virtual void set_string(const std::string &key, const std::string &value) = 0;
	virtual void set_string_list(const std::string &key, const std::vector<std::string> &value) = 0;
	virtual void set_records(const std::string &key, const std::vector<SettingsRecord> &value) = 0;
};

// Storage keys, so the store, the tests and later migrations agree. Each is
// the snake_case field name ("clips_folder", "grace_period_seconds", ...).
// source_selection is stored as the string "automatic" or "chosen".
// game_rules is stored as records with keys "executable", "display_name"
// and "ignore" ("true"/"false").
namespace settings_keys {
constexpr const char *kSchemaVersion = "schema_version";
constexpr const char *kClipsFolder = "clips_folder";
constexpr const char *kGameFolders = "game_folders";
constexpr const char *kMonthlyFolders = "monthly_folders";
constexpr const char *kUnsortedFolder = "unsorted_folder";
constexpr const char *kAutoSort = "auto_sort";
constexpr const char *kSortReplays = "sort_replays";
constexpr const char *kSortRecordings = "sort_recordings";
constexpr const char *kSortScreenshots = "sort_screenshots";
constexpr const char *kReplayAutoStart = "replay_auto_start";
constexpr const char *kReplayAutoStop = "replay_auto_stop";
constexpr const char *kGracePeriodSeconds = "grace_period_seconds";
constexpr const char *kPlaySaveSound = "play_save_sound";
constexpr const char *kSaveSoundPath = "save_sound_path";
constexpr const char *kSaveSoundVolume = "save_sound_volume";
constexpr const char *kSourceSelection = "source_selection";
constexpr const char *kChosenSources = "chosen_sources";
constexpr const char *kUseProductName = "use_product_name";
constexpr const char *kRecordingDominantGame = "recording_dominant_game";
constexpr const char *kGameRules = "game_rules";
constexpr const char *kCheckForUpdates = "check_for_updates";
} // namespace settings_keys

// Brings any settings into range. Idempotent.
//   - clips_folder: surrounding whitespace trimmed (an empty result is valid)
//   - unsorted_folder: sanitize_folder_name(value, "Unsorted")
//   - grace_period_seconds: clamped to [kMinGracePeriodSeconds, kMaxGracePeriodSeconds]
//   - save_sound_path: surrounding whitespace trimmed
//   - save_sound_volume: clamped to [kMinSaveSoundVolume, kMaxSaveSoundVolume]
//   - chosen_sources: blanks removed, duplicates removed keeping first order
//   - game_rules: executable trimmed and ASCII-lowercased; rules with an empty
//     executable dropped; for duplicate executables the first rule wins;
//     display_name trimmed
//   - schema_version: set to kSchemaVersion
Settings validated(Settings settings);

// Reads every key, keeping the default for anything missing or mistyped,
// runs migrate_settings() if the stored schema_version is older, then
// validated(). A stored version newer than kSchemaVersion is read as-is
// (unknown keys ignored) so a downgrade keeps what it understands.
Settings load_settings(const SettingsReader &reader);

// Writes every field, including schema_version = kSchemaVersion.
void save_settings(const Settings &settings, SettingsWriter &writer);

// Upgrades settings read from schema `from_version` to kSchemaVersion. The
// reader is passed so a migration can look at keys that no longer exist.
// Version 1 is the first schema, so for now this only exists as the hook.
void migrate_settings(Settings &settings, int from_version, const SettingsReader &reader);

} // namespace clip_atlas
