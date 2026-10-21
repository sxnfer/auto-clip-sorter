// Clip Atlas — finds capture sources and turns their signals into game-state
// events.
//
// Discovery is event driven, with no polling:
//   - global signals on obs_get_signal_handler():
//       "source_create"  (ptr source)
//       "source_destroy" (ptr source)  fired on OBS's destruction thread
//       "source_rename"  (ptr source, string new_name, string prev_name)
//   - frontend events OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING (forget
//     everything: GameState::clear) and OBS_FRONTEND_EVENT_FINISHED_LOADING /
//     OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED (obs_enum_sources() to pick
//     up anything created before we connected).
//
// A source is tracked when obs_source_get_unversioned_id() is "game_capture"
// or "window_capture" and, if Settings::source_selection is chosen, its UUID
// (obs_source_get_uuid()) is in Settings::chosen_sources. Both source types
// declare, in OBS 31.0 plugins/win-capture:
//   signal "void hooked(ptr source, string title, string class, string executable)"
//   signal "void unhooked(ptr source)"
//   proc   "void get_hooked(out bool hooked, out string title, out string class,
//                           out string executable)"
// plus libobs's per-source "activate"/"deactivate" (ptr source), which mean
// the source became visible / hidden in the Program output. "show"/"hide" are
// deliberately not used: they also fire for previews and projectors.
//
// When a source starts being tracked, its current state is read once with
// proc_handler_call(obs_source_get_proc_handler(src), "get_hooked", &cd) and
// obs_source_active(src), so a game that was already running is picked up.
//
// Threading: hooked/unhooked and activate/deactivate arrive on the graphics
// thread (win-capture's and libobs's video ticks); source_destroy on OBS's
// destruction thread, before the source's own destroy; source_create and
// source_rename on whichever thread creates or renames the source. Callbacks
// copy what they need by value and hop with run_on_ui(); GameState is only
// touched on the UI thread. get_hooked reads strings win-capture reallocates
// on the graphics thread, so the UI thread never calls it: it queues an
// OBS_TASK_GRAPHICS task that reads the state and hands it back with a strong
// reference, which the UI thread releases. Otherwise only obs_weak_source_t
// references are held. stop() waits for OBS's destroy queue, then disconnects
// everything; signal_handler_disconnect() waits for an in-flight callback of
// that signal, so per-source data may be freed after it.
//
// The frontend callback is never removed (OBS dispatches by index, so a
// removal during EXIT would make another callback run twice or be skipped);
// it goes inert after stop(). Frontend events are not delivered while OBS's
// disableSaving counter is set, so they are a backstop: source_create and
// source_destroy carry the real work.
//
// UI thread only (all public functions).
#pragma once

#include "game-state.hpp"
#include "settings-model.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace clip_atlas {

struct TrackedSource {
	std::string uuid;
	std::string name;
	bool hooked = false;
	bool live = false;
	HookInfo info; // valid while hooked
};

class SourceTracker {
public:
	// Called on the UI thread after every event that changed
	// GameState::current().
	using ChangedCallback = std::function<void()>;
	// True if hook info belongs to an executable the user ignores; such hooks
	// are fed to GameState as unhooked so they never become the current game.
	using IgnorePredicate = std::function<bool(const HookInfo &)>;

	SourceTracker(GameState &state, ChangedCallback on_changed, IgnorePredicate is_ignored);
	~SourceTracker(); // calls stop()

	SourceTracker(const SourceTracker &) = delete;
	SourceTracker &operator=(const SourceTracker &) = delete;

	// Connects the global signals and frontend callback, then tracks every
	// matching existing source. Call once from obs_module_load().
	void start(const Settings &settings);

	// Disconnects everything and forgets all sources. Idempotent. Call from
	// obs_module_unload().
	void stop();

	// Re-evaluates which sources are tracked after source_selection or
	// chosen_sources changed, connecting and disconnecting as needed.
	void apply_settings(const Settings &settings);

	// Snapshot for the UI (settings dialog, dock).
	std::vector<TrackedSource> tracked_sources() const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace clip_atlas
