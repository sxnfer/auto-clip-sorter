// Clip Atlas — the "Toggle Clip Atlas auto-sort" hotkey. A press flips Settings::auto_sort, the same switch as the dock
// header's toggle.
//
// OBS keeps a plugin's bindings in the profile. The Settings dialog saves them under the hotkey's name, and OBS loads
// only its own, so the plugin reads its binding back in start() and in on_profile_changed(). hotkeys.hpp lists the OBS
// facts this relies on.
//
// OBS may call the callback until the hotkey is unregistered, and a press queued to the UI thread just before stop()
// can still run after this object is gone. The Handle the callback reads is therefore never freed: hotkey_handles()
// keeps a reference to it for the life of the process, and stop() makes it inert.
//
// UI thread only. OBS routes hotkey callbacks to the UI thread, so nothing here needs a lock.
#include "hotkeys.hpp"

#include <obs-data.h>
#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>
#include <util/config-file.h>

#include <deque>
#include <exception>
#include <memory>

namespace clip_atlas {

namespace {

// What the hotkey callback may reach. `store` is the caller's, and Hotkeys must not outlive it. The callback reads it
// only while `active` is set, and stop() clears `active`.
struct Handle {
	SettingsStore *store = nullptr;
	bool active = true;
};

// Keeps every Handle alive for the life of the process. OBS holds the pointer until the hotkey is unregistered, and a
// queued press can run after that, so a Handle must not be freed while the plugin is loaded. frontend-events.cpp keeps
// its handles in a deque for the same reason. There is one entry per Hotkeys object, which is one per plugin load.
std::deque<std::shared_ptr<Handle>> &hotkey_handles()
{
	static std::deque<std::shared_ptr<Handle>> handles;
	return handles;
}

// The public methods run inside OBS event handlers. An exception unwinding through OBS's C code would take OBS down,
// so it is logged here and the next event carries on.
template<typename Fn> void run_guarded(const char *where, Fn &&fn)
{
	try {
		fn();
	} catch (const std::exception &e) {
		obs_log(LOG_ERROR, "auto-sort hotkey: %s failed: %s", where, e.what());
	} catch (...) {
		obs_log(LOG_ERROR, "auto-sort hotkey: %s failed: unknown exception", where);
	}
}

// Owners for the OBS objects that load_binding() gets. A null value is fine, since unique_ptr does not call its
// deleter for null.
using DataPtr = std::unique_ptr<obs_data_t, void (*)(obs_data_t *)>;
using ArrayPtr = std::unique_ptr<obs_data_array_t, void (*)(obs_data_array_t *)>;

// Loads the binding saved for this hotkey in the current profile. OBS writes it as {"bindings": [...]} under the
// hotkey's name in the profile's [Hotkeys] section. A missing value, or one that can't be parsed, leaves the hotkey
// unbound.
void load_binding(obs_hotkey_id id)
{
	// Owned by OBS, so it is not freed here. It is null only when no profile is open.
	config_t *profile = obs_frontend_get_profile_config();
	const char *json = profile ? config_get_string(profile, "Hotkeys", kToggleAutoSortName) : nullptr;
	const DataPtr data(json ? obs_data_create_from_json(json) : nullptr, obs_data_release);
	if (json && !data)
		obs_log(LOG_WARNING,
			"the saved binding of the auto-sort hotkey is not valid JSON; the hotkey stays unbound");
	const ArrayPtr bindings(data ? obs_data_get_array(data.get(), "bindings") : nullptr, obs_data_array_release);
	if (!bindings) {
		// Unbinding goes through obs_hotkey_load_bindings(), which clears the old bindings. That avoids passing
		// obs_hotkey_load() a null array.
		obs_hotkey_load_bindings(id, nullptr, 0);
		return;
	}
	obs_hotkey_load(id, bindings.get());
}

// The hotkey's callback. OBS calls it on press and on release, and only a press toggles. The settings listener moves
// the dock's switch, so nothing here touches the dock.
void on_hotkey(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	run_guarded("press", [data, pressed] {
		if (!pressed)
			return;
		Handle &handle = *static_cast<Handle *>(data);
		if (!handle.active)
			return;
		// A copy: get() returns a reference that set() replaces.
		Settings next = handle.store->get();
		next.auto_sort = !next.auto_sort;
		handle.store->set(next);
		if (next.auto_sort)
			obs_log(LOG_INFO, "auto-sort turned on by hotkey");
		else
			obs_log(LOG_INFO, "auto-sort turned off by hotkey");
	});
}

} // namespace

struct Hotkeys::Impl {
	explicit Impl(SettingsStore &settings);

	void start();
	void on_profile_changed();
	void stop();

	// Shared with hotkey_handles(), which holds a second reference, so the Handle outlives this Impl.
	std::shared_ptr<Handle> handle;
	obs_hotkey_id id = OBS_INVALID_HOTKEY_ID; // set only while registered
	bool started = false;                     // start() runs once, even when registration fails
};

Hotkeys::Impl::Impl(SettingsStore &settings) : handle(std::make_shared<Handle>())
{
	handle->store = &settings;
	hotkey_handles().push_back(handle);
}

void Hotkeys::Impl::start()
{
	// Once only, even after a failed registration: a second attempt would repeat the same warning.
	if (started)
		return;
	started = true;
	if (!handle->active)
		return;
	const obs_hotkey_id registered = obs_hotkey_register_frontend(
		kToggleAutoSortName, obs_module_text("Hotkey.ToggleAutoSort"), on_hotkey, handle.get());
	if (registered == OBS_INVALID_HOTKEY_ID) {
		obs_log(LOG_WARNING, "could not register the auto-sort hotkey; it will not work this session");
		return;
	}
	id = registered;
	load_binding(id);
	obs_log(LOG_INFO, "toggle auto-sort hotkey registered");
}

void Hotkeys::Impl::on_profile_changed()
{
	// Each profile has its own bindings, so the new profile's replace the old ones.
	if (!handle->active || id == OBS_INVALID_HOTKEY_ID)
		return;
	load_binding(id);
}

void Hotkeys::Impl::stop()
{
	// Cleared even when there is nothing to unregister, so a start() after stop() does nothing.
	handle->active = false;
	if (id == OBS_INVALID_HOTKEY_ID)
		return;
	obs_hotkey_unregister(id);
	id = OBS_INVALID_HOTKEY_ID;
}

Hotkeys::Hotkeys(SettingsStore &settings) : impl_(std::make_unique<Impl>(settings)) {}

Hotkeys::~Hotkeys()
{
	stop();
}

void Hotkeys::start()
{
	run_guarded("start", [this] { impl_->start(); });
}

void Hotkeys::on_profile_changed()
{
	run_guarded("profile change", [this] { impl_->on_profile_changed(); });
}

void Hotkeys::stop()
{
	run_guarded("stop", [this] { impl_->stop(); });
}

} // namespace clip_atlas
