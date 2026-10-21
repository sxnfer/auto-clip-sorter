/*
Clip Atlas
Copyright (C) 2026 sxnfer

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>

#include "clip-dock.hpp"
#include "clip-filer.hpp"
#include "clip-routing.hpp"
#include "file-mover.hpp"
#include "frontend-events.hpp"
#include "game-state.hpp"
#include "name-resolver.hpp"
#include "recording-events.hpp"
#include "replay-controller.hpp"
#include "settings-dialog.hpp"
#include "settings-model.hpp"
#include "settings-store.hpp"
#include "shell-ops.hpp"
#include "sound-player.hpp"
#include "source-tracker.hpp"
#include "thumbnails.hpp"
#include "ui-thread.hpp"

#include <QPointer>
#include <QWidget>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <vector>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

namespace clip_atlas {

namespace {

// Everything the plugin owns while it is loaded. The members are destroyed in reverse order, so the frontend
// and replay code, which refer to the state and the mover, go before them.
struct PluginContext {
	GameState state;
	FileMover mover;
	std::unique_ptr<SourceTracker> tracker;
	std::unique_ptr<ReplayController> replay;
	std::unique_ptr<FrontendEvents> events;
	std::unique_ptr<RecordingEvents> recordings;
	int settings_listener = 0;
};

// Made by start_plugin(). Destroyed only by obs_module_unload(), through release_context().
std::unique_ptr<PluginContext> g_context;

// The context once obs_module_unload() has found that OBS may still call into it (no EXIT was seen). It stays alive
// until the process exits, since OBS never unloads plugin code. Only release_context() sets it.
std::unique_ptr<PluginContext> g_retained;

// Set by stop_all(). Once it is set, nothing starts again.
bool g_stopped = false;

// Set when OBS_FRONTEND_EVENT_EXIT arrives. After EXIT, OBS has nulled every frontend callback.
bool g_exit_seen = false;

// Set once start_replay_control() has started the Replay Buffer controller.
bool g_replay_started = false;

// The clip browser. OBS owns the widget once it is added, and deletes it with the main window, so the plugin only looks
// at it through this pointer, which clears itself when the dock is gone. The plugin never deletes it.
QPointer<ClipDock> g_dock;

// The save ids whose save sound has played, newest last. The files of one save are reported close together, so the
// last kRememberedSaves ids are enough to recognise a second file of the same save. UI thread only.
constexpr std::size_t kRememberedSaves = 32;
std::deque<std::uint64_t> g_played_saves;

// The user's game rules as the resolver's map. frontend-events.cpp builds the same map from Settings.
UserGameRules user_rules(const std::vector<GameRuleSetting> &rules)
{
	UserGameRules out;
	for (const GameRuleSetting &rule : rules)
		out.emplace(rule.executable, UserGameRule{rule.display_name, rule.ignore});
	return out;
}

// SourceTracker's ignore test. It reads the rules as they are now, so an edit applies from the next hook.
bool hook_is_ignored(const HookInfo &info)
{
	try {
		const ResolvedGame game = resolve_game({info.executable, info.title, ""},
						       user_rules(settings_store().get().game_rules),
						       BundledGameNames{});
		return game.ignored;
	} catch (const std::exception &e) {
		obs_log(LOG_ERROR, "could not check the ignore rules: %s", e.what());
	} catch (...) {
		obs_log(LOG_ERROR, "could not check the ignore rules: unknown exception");
	}
	return false;
}

// SourceTracker says the current game may have changed. The replay controller decides whether the Replay Buffer
// should start or stop. Changes before the controller starts are not forwarded: start_replay_control() tells it
// the current game once it has started. The recording clock gets every change while the plugin runs. The clip
// browser shows the game in its header, so it gets every change too.
void on_game_changed()
{
	try {
		PluginContext *ctx = g_context.get();
		if (ctx && ctx->replay && g_replay_started)
			ctx->replay->on_game_changed();
	} catch (const std::exception &e) {
		obs_log(LOG_ERROR, "replay buffer update failed: %s", e.what());
	} catch (...) {
		obs_log(LOG_ERROR, "replay buffer update failed: unknown exception");
	}
	// A separate try, so a failure above cannot skip this. Recordings do not depend on the Replay Buffer controller.
	try {
		PluginContext *ctx = g_context.get();
		if (ctx && ctx->recordings && !g_stopped)
			ctx->recordings->on_game_changed();
	} catch (const std::exception &e) {
		obs_log(LOG_ERROR, "recording game update failed: %s", e.what());
	} catch (...) {
		obs_log(LOG_ERROR, "recording game update failed: unknown exception");
	}
	// Separate again, so neither failure above can skip the header.
	try {
		if (!g_stopped && g_dock)
			g_dock->on_game_changed();
	} catch (const std::exception &e) {
		obs_log(LOG_ERROR, "clip browser game update failed: %s", e.what());
	} catch (...) {
		obs_log(LOG_ERROR, "clip browser game update failed: unknown exception");
	}
}

// Settings were saved. Pushes the new values to every component that uses them. The replay controller gets them
// only once it has started; start_replay_control() passes it the settings as they are at that point.
void apply_settings_change(const Settings &settings)
{
	PluginContext *ctx = g_context.get();
	if (!ctx)
		return;
	// First, so an exception from the tracker or the replay controller cannot skip it. A rule change can make the
	// current game ignored, so a recording in progress must see the new rules.
	if (ctx->recordings)
		ctx->recordings->on_game_changed();
	ctx->state.set_grace_period(std::chrono::seconds(settings.grace_period_seconds));
	if (ctx->tracker)
		ctx->tracker->apply_settings(settings);
	if (ctx->replay && g_replay_started)
		ctx->replay->apply_settings(settings);
	// Last, in its own try: the dock follows the new folder, auto-sort and replay settings.
	try {
		if (!g_stopped && g_dock)
			g_dock->apply_settings(settings);
	} catch (const std::exception &e) {
		obs_log(LOG_ERROR, "clip browser settings update failed: %s", e.what());
	} catch (...) {
		obs_log(LOG_ERROR, "clip browser settings update failed: unknown exception");
	}
}

// Tools → Clip Atlas Settings…. OBS calls it on the UI thread. Once the plugin is stopped it does nothing, because
// the context can outlive EXIT until unload.
void open_settings_from_menu(void *)
{
	try {
		if (g_stopped || !g_context)
			return;
		open_settings_dialog(settings_store(), static_cast<QWidget *>(obs_frontend_get_main_window()));
	} catch (const std::exception &e) {
		obs_log(LOG_ERROR, "could not open settings: %s", e.what());
	} catch (...) {
		obs_log(LOG_ERROR, "could not open settings: unknown exception");
	}
}

// Stops every component and worker, but destroys nothing. OBS may still call the plugin's frontend callbacks until it
// has sent EXIT and nulled them, and a callback that runs in the meantime only finds stopped objects, which do
// nothing. Runs from the EXIT handler, and again from obs_module_unload(), where it does nothing. Never throws.
void stop_all()
{
	if (g_stopped)
		return;
	g_stopped = true;
	try {
		ui_tasks_shutdown();
		sound_player().shutdown();
		close_settings_dialog();
		if (PluginContext *ctx = g_context.get()) {
			settings_store().unsubscribe(ctx->settings_listener);
			if (ctx->events)
				ctx->events->stop();
			if (ctx->recordings)
				ctx->recordings->stop();
			// Only a started controller has anything registered to stop.
			if (ctx->replay && g_replay_started)
				ctx->replay->stop();
			if (ctx->tracker)
				ctx->tracker->stop();
			// The dock first: it stops its own queue and makes its results do nothing, so nothing is left
			// waiting on the thumbnails or the Shell. Both are singletons that are never destroyed, and
			// shutdown() is idempotent.
			if (g_dock)
				g_dock->stop();
			thumbnail_provider().shutdown();
			shell_ops().shutdown();
			ctx->mover.shutdown(false);
		}
	} catch (const std::exception &e) {
		obs_log(LOG_ERROR, "stopping the plugin failed: %s", e.what());
	} catch (...) {
		obs_log(LOG_ERROR, "stopping the plugin failed: unknown exception");
	}
}

// Destroys the context. Only after OBS has sent EXIT, because only then has OBS nulled the frontend callbacks, so
// nothing can call into the objects. Without EXIT, OBS may still call them, so the context is kept alive instead.
void release_context()
{
	if (!g_context)
		return;
	if (g_exit_seen)
		g_context.reset();
	else
		g_retained = std::move(g_context);
}

// OBS_FRONTEND_EVENT_FINISHED_LOADING. Starts the Replay Buffer controller once, with the settings as they are now,
// then tells it the current game, since game changes before this point were not forwarded. Logs the folder replays
// are sorted into. That folder can only be read now, once OBS's recording folder exists.
void start_replay_control()
{
	PluginContext *ctx = g_context.get();
	if (g_stopped || !ctx || g_replay_started)
		return;
	g_replay_started = true;
	try {
		if (ctx->replay) {
			ctx->replay->start(settings_store().get());
			ctx->replay->on_game_changed();
		}
	} catch (const std::exception &e) {
		obs_log(LOG_ERROR, "replay buffer control did not start: %s", e.what());
	} catch (...) {
		obs_log(LOG_ERROR, "replay buffer control did not start: unknown exception");
	}
	const std::string folder = settings_store().effective_clips_folder();
	obs_log(LOG_INFO, "clips folder: %s",
		folder.empty() ? "none (replays are left where OBS saves them)" : folder.c_str());
}

// True the first time `save_id` is seen. Only the last kRememberedSaves ids are kept. Id 0 means the producer set none,
// so it can't be matched against anything and always counts as new.
bool is_new_save(std::uint64_t save_id)
{
	if (save_id == 0)
		return true;
	if (std::find(g_played_saves.begin(), g_played_saves.end(), save_id) != g_played_saves.end())
		return false;
	g_played_saves.push_back(save_id);
	while (g_played_saves.size() > kRememberedSaves)
		g_played_saves.pop_front();
	return true;
}

// The filing code calls this on the UI thread, once for each file it has filed. A remuxed replay gives two files with
// the same save_id, and the sound must play once for the save. plays_save_sound() decides whether a file counts: it
// must be a replay that was moved or copied into its folder, with the sound switched on. The id is remembered only
// when the sound would play, so a file that was skipped never blocks its save's sound.
void on_clip_sorted(const ClipSorted &clip)
{
	// The dock first, in its own try, so a failure there cannot skip the save sound below.
	try {
		if (!g_stopped && g_dock)
			g_dock->on_clip_sorted(clip);
	} catch (const std::exception &e) {
		obs_log(LOG_ERROR, "clip browser update failed: %s", e.what());
	} catch (...) {
		obs_log(LOG_ERROR, "clip browser update failed: unknown exception");
	}
	try {
		if (g_stopped)
			return;
		const bool filed = clip.status == MoveStatus::moved || clip.status == MoveStatus::copied_source_kept;
		const Settings settings = settings_store().get();
		if (!plays_save_sound(settings, clip.kind, filed) || !is_new_save(clip.save_id))
			return;
		sound_player().play(settings.save_sound_path, settings.save_sound_volume, [](const std::string &error) {
			if (!error.empty())
				obs_log(LOG_WARNING, "save sound did not play: %s", error.c_str());
		});
	} catch (const std::exception &e) {
		obs_log(LOG_ERROR, "save sound failed: %s", e.what());
	} catch (...) {
		obs_log(LOG_ERROR, "save sound failed: unknown exception");
	}
}

// Plugin-level frontend callback, registered by obs_module_load() and never removed. It handles two events:
//   - OBS_FRONTEND_EVENT_FINISHED_LOADING: OBS has created its outputs and scene collection, so the Replay Buffer
//     controller starts here. Module load is too early, because the controller asks OBS for the replay buffer output.
//     The clip browser starts here too, because OBS's recording folder can only be read now.
//   - OBS_FRONTEND_EVENT_PROFILE_CHANGED: the clip browser checks whether its folder or replay prefix changed.
//   - OBS_FRONTEND_EVENT_EXIT: OBS is shutting down. The plugin stops here. Objects are destroyed later, in
//     obs_module_unload().
// Its private data is nullptr, which stays valid for the life of the process.
void on_plugin_frontend_event(obs_frontend_event event, void *)
{
	try {
		if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
			start_replay_control();
			// Own try, so a failure in the dock cannot skip the rest of this event.
			try {
				if (!g_stopped && g_dock) {
					g_dock->start();
					// The dock does not own the Replay Buffer controller, so the current status is
					// passed on here.
					PluginContext *ctx = g_context.get();
					if (ctx && ctx->replay)
						g_dock->on_replay_status(ctx->replay->status());
					g_dock->on_game_changed();
				}
			} catch (const std::exception &e) {
				obs_log(LOG_ERROR, "clip browser did not start: %s", e.what());
			} catch (...) {
				obs_log(LOG_ERROR, "clip browser did not start: unknown exception");
			}
		} else if (event == OBS_FRONTEND_EVENT_PROFILE_CHANGED) {
			// Own try, for the same reason as above.
			try {
				if (!g_stopped && g_dock)
					g_dock->on_profile_changed();
			} catch (const std::exception &e) {
				obs_log(LOG_ERROR, "clip browser profile update failed: %s", e.what());
			} catch (...) {
				obs_log(LOG_ERROR, "clip browser profile update failed: unknown exception");
			}
		} else if (event == OBS_FRONTEND_EVENT_EXIT) {
			g_exit_seen = true;
			stop_all();
		}
	} catch (const std::exception &e) {
		obs_log(LOG_ERROR, "frontend event failed: %s", e.what());
	} catch (...) {
		obs_log(LOG_ERROR, "frontend event failed: unknown exception");
	}
}

// Builds the plugin for obs_module_load(). Throws on failure; the caller logs it and calls stop_all().
void start_plugin()
{
	settings_store().load();
	const Settings settings = settings_store().get();

	g_context = std::make_unique<PluginContext>();
	PluginContext &ctx = *g_context;
	ctx.state.set_grace_period(std::chrono::seconds(settings.grace_period_seconds));

	// Only signal connections and frontend callbacks happen here. OBS's outputs do not exist yet, so the replay
	// controller is not started until FINISHED_LOADING (see start_replay_control()).
	ctx.tracker = std::make_unique<SourceTracker>(ctx.state, on_game_changed, hook_is_ignored);
	ctx.replay = std::make_unique<ReplayController>(ctx.state);
	ctx.events = std::make_unique<FrontendEvents>(ctx.state, settings_store(), ctx.mover);
	ctx.recordings = std::make_unique<RecordingEvents>(ctx.state, settings_store(), ctx.mover);

	ctx.tracker->start(settings);
	ctx.events->set_clip_sorted_callback(on_clip_sorted);
	ctx.events->start();
	ctx.recordings->set_clip_sorted_callback(on_clip_sorted);
	ctx.recordings->start();

	ctx.settings_listener =
		settings_store().subscribe([](const Settings &changed) { apply_settings_change(changed); });

	obs_frontend_add_tools_menu_item(obs_module_text("SettingsMenuItem"), open_settings_from_menu, nullptr);

	// The dock shows the Replay Buffer's status, so it hears about every change from here on. Set before the dock
	// exists, so the callback never runs against a dock that is not there yet.
	ctx.replay->set_status_callback([](ReplayBufferStatus status) {
		try {
			if (!g_stopped && g_dock)
				g_dock->on_replay_status(status);
		} catch (const std::exception &e) {
			obs_log(LOG_ERROR, "clip browser status update failed: %s", e.what());
		} catch (...) {
			obs_log(LOG_ERROR, "clip browser status update failed: unknown exception");
		}
	});
	// OBS restores the user's dock layout after the modules have loaded, so the dock is added here. It is optional:
	// if it cannot be created, everything else keeps working.
	try {
		g_dock = register_clip_dock(ctx.state, settings_store(), ctx.mover);
	} catch (const std::exception &e) {
		obs_log(LOG_ERROR, "clip browser did not load: %s", e.what());
	} catch (...) {
		obs_log(LOG_ERROR, "clip browser did not load: unknown exception");
	}

	// Registered last. The callback is never removed, so nothing that can fail may come after this step.
	obs_frontend_add_event_callback(on_plugin_frontend_event, nullptr);
}

} // namespace
} // namespace clip_atlas

// OBS calls these from C code, so an exception must not unwind into it. Catch and log instead.
bool obs_module_load(void)
{
	try {
		clip_atlas::start_plugin();
		obs_log(LOG_INFO, "loaded v%s", PLUGIN_VERSION);
		return true;
	} catch (const std::exception &e) {
		obs_log(LOG_ERROR, "load failed: %s", e.what());
	} catch (...) {
		obs_log(LOG_ERROR, "load failed: unknown exception");
	}
	// Stops what was started, but keeps the objects: OBS may still call the callbacks they registered.
	clip_atlas::stop_all();
	return false;
}

void obs_module_unload(void)
{
	try {
		// stop_all() does nothing when the EXIT handler already ran, which is the usual order.
		clip_atlas::stop_all();
		clip_atlas::release_context();
		obs_log(LOG_INFO, "unloaded");
	} catch (const std::exception &e) {
		obs_log(LOG_ERROR, "unload failed: %s", e.what());
	} catch (...) {
		obs_log(LOG_ERROR, "unload failed: unknown exception");
	}
}

const char *obs_module_name(void)
{
	return obs_module_text("ModuleName");
}

const char *obs_module_description(void)
{
	return obs_module_text("ModuleDescription");
}
