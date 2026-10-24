// Clip Atlas — sorts saved replays and screenshots into game folders.
//
// One frontend callback (UI thread), registered by start() and never removed:
//
// OBS_FRONTEND_EVENT_REPLAY_BUFFER_SAVED:
//   1. path = obs_frontend_get_last_replay() (char *, free with bfree()).
//      OBS sets it before the event, then calls AutoRemux(path).
//   2. Skip (log at INFO) unless sorting_enabled(settings, ClipKind::replay).
//   3. game = clip_game_for(GameState::game_for_save(now), settings).
//   4. filer.file(ClipKind::replay, {{path, expected_remux_path(path,
//      read_remux_config())}}, game).
//
// OBS_FRONTEND_EVENT_SCREENSHOT_TAKEN (M2):
//   1. path = obs_frontend_get_last_screenshot() (char *, free with bfree()).
//      OBS joins its writer thread and sets the path before the event
//      (UI/window-basic-main-screenshot.cpp), so the file is complete.
//   2. Skip (log at INFO) unless sorting_enabled(settings, ClipKind::screenshot).
//   3. game = clip_game_for(GameState::game_for_save(now), settings).
//   4. filer.file(ClipKind::screenshot, {{path, {}}}, game): OBS never remuxes
//      a screenshot.
// A missing or empty path is logged as a warning and nothing is filed.
//
// Replays and screenshots share one ClipFiler, so their waits run in order.
//
// The callback's private data is a handle that outlives FrontendEvents (OBS
// keeps every callback until the process exits); once FrontendEvents is
// stopped or gone, the callback does nothing.
//
// UI thread only.
#pragma once

#include "clip-filer.hpp"
#include "file-mover.hpp"
#include "game-state.hpp"
#include "settings-store.hpp"

#include <memory>

namespace clip_atlas {

class FrontendEvents {
public:
	// Called on the UI thread once per file (a remuxed pair gives two calls,
	// with the same save_id).
	using ClipSortedCallback = clip_atlas::ClipSortedCallback;

	FrontendEvents(const GameState &state, SettingsStore &settings, FileMover &mover);
	~FrontendEvents(); // calls stop()

	FrontendEvents(const FrontendEvents &) = delete;
	FrontendEvents &operator=(const FrontendEvents &) = delete;

	// obs_frontend_add_event_callback, once. The callback is never removed.
	// A start() after stop() does nothing.
	void start();
	// Idempotent. Makes the callback inert and stops the filer; results of
	// jobs still in flight are dropped.
	void stop();

	void set_clip_sorted_callback(ClipSortedCallback callback);
	void set_activity_callback(ActivityCallback callback);

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace clip_atlas
