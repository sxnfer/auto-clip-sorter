// Clip Atlas — the plugin's settings, persisted as JSON in OBS's plugin config
// folder and shared with every part of the plugin.
//
// File: obs_module_config_path("settings.json") (char *, free with bfree()).
// The folder may not exist yet; create it with os_mkdirs(). Writes go through
// obs_data_save_json_safe(data, path, "tmp", "bak"), which writes a temporary
// file and renames it over the old one, so a crash mid-write can't corrupt it.
//
// UI thread only. Reading and writing this one small file on the UI thread is
// an accepted exception to the "no file I/O on the UI thread" rule.
#pragma once

#include "settings-model.hpp"

#include <functional>
#include <map>
#include <string>

namespace clip_atlas {

class SettingsStore {
public:
	using Listener = std::function<void(const Settings &)>;

	// Loads settings.json. A missing file gives defaults. A file that can't
	// be parsed (obs_data_create_from_json_file returns NULL) is renamed to
	// "settings.json.bad" (replacing an older .bad), logged as a warning, and
	// defaults are used.
	void load();

	// The current settings (defaults before load()).
	const Settings &get() const;

	// validated(settings) becomes current, is saved, and every listener is
	// called with it, in subscription order. A save failure is logged and the
	// new settings still apply for this session. Returns whether the save
	// succeeded.
	bool set(const Settings &settings);

	// Returns an id for unsubscribe(). Listeners are not called on subscribe.
	int subscribe(Listener listener);
	void unsubscribe(int id);

	// settings.clips_folder if not empty, otherwise OBS's current recording
	// folder from obs_frontend_get_current_record_output_path() (char *, free
	// with bfree(); may be NULL or empty, giving ""). UTF-8.
	std::string effective_clips_folder() const;

private:
	Settings settings_;
	std::map<int, Listener> listeners_;
	int next_listener_id_ = 1;
};

// The plugin's single store, created in obs_module_load().
SettingsStore &settings_store();

} // namespace clip_atlas
