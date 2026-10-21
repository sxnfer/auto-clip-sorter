// Clip Atlas — the plugin's hotkey: "Toggle Clip Atlas auto-sort", listed in
// OBS's Settings → Hotkeys.
//
// OBS 31 facts this relies on:
//   - obs_hotkey_register_frontend(const char *name, const char *description,
//     obs_hotkey_func func, void *data) returns an obs_hotkey_id
//     (OBS_INVALID_HOTKEY_ID on failure); obs_hotkey_unregister(id) removes it.
//     obs_hotkey_func is void (*)(void *data, obs_hotkey_id id,
//     obs_hotkey_t *hotkey, bool pressed) (libobs/obs-hotkey.h:143-187).
//   - The frontend routes hotkey callbacks to the UI thread
//     (obs_hotkey_set_callback_routing_func(OBSBasic::HotkeyTriggered),
//     which uses QMetaObject::invokeMethod; UI/window-basic-main.cpp:2629),
//     so the callback runs on the UI thread.
//   - OBS's Settings dialog saves every frontend hotkey's bindings, plugins'
//     included, in the profile config: section "Hotkeys", key = the hotkey's
//     name, value = JSON {"bindings": [...]} from obs_hotkey_save(id)
//     (UI/window-basic-settings.cpp:3581-3603). OBS loads only its own hotkeys
//     from there (window-basic-main.cpp:2642-2660), so the plugin loads its
//     own: config_get_string(obs_frontend_get_profile_config(), "Hotkeys",
//     kToggleAutoSortName) → obs_data_create_from_json → obs_data_get_array(
//     data, "bindings") → obs_hotkey_load(id, array) (release the array and the
//     data). A missing value leaves the hotkey unbound.
//
// The callback, on `pressed` only, flips Settings::auto_sort through
// SettingsStore::set() (the dock's switch follows through the settings
// listener) and logs the new state at INFO. It does nothing after stop(). Its
// `data` pointer is a heap handle that outlives this object, because OBS may
// call it until the hotkey is unregistered.
//
// UI thread only.
#pragma once

#include "settings-store.hpp"

#include <memory>

namespace clip_atlas {

// The hotkey's name in OBS (and in the profile's [Hotkeys] section).
inline constexpr const char *kToggleAutoSortName = "clip_atlas.toggle_auto_sort";

class Hotkeys {
public:
	explicit Hotkeys(SettingsStore &settings);
	// Calls stop().
	~Hotkeys();

	Hotkeys(const Hotkeys &) = delete;
	Hotkeys &operator=(const Hotkeys &) = delete;

	// Registers the hotkey with obs_module_text("Hotkey.ToggleAutoSort") as
	// its description and loads its bindings from the profile. Call at
	// OBS_FRONTEND_EVENT_FINISHED_LOADING (the profile config exists then).
	// Once only; does nothing after stop().
	void start();

	// OBS_FRONTEND_EVENT_PROFILE_CHANGED: each profile has its own bindings,
	// so the new profile's are loaded (an unbound profile clears them with
	// obs_hotkey_load_bindings(id, nullptr, 0)).
	void on_profile_changed();

	// Unregisters the hotkey (OBS saved its bindings when the user set them)
	// and makes the callback inert. Idempotent. Call at EXIT, while libobs is
	// still running.
	void stop();

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace clip_atlas
