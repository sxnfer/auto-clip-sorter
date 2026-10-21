// Clip Atlas — sorts saved replays into game folders.
//
// On OBS_FRONTEND_EVENT_REPLAY_BUFFER_SAVED (UI thread):
//   1. path = obs_frontend_get_last_replay() (char *, free with bfree()).
//   2. Skip if Settings::auto_sort or sort_replays is off.
//   3. game = GameState::game_for_save(now), resolved with resolve_game();
//      an ignored or missing game routes to the unsorted folder.
//   4. dest = destination_dir(effective_clips_folder(), display_name,
//      year_month_of(system_clock::now()), {monthly_folders, unsorted_folder}).
//   5. OBS may remux the replay right after this event (OBSBasic::AutoRemux).
//      If expected_remux_path() is non-empty, queue
//      FileMover::wait_stable(remuxed, 3 s, 10 min) and only then move the
//      original and the remuxed file (if it appeared) into dest. Otherwise
//      move the original at once.
//   RemuxConfig comes from obs_frontend_get_profile_config() (config_t *, not
//   owned): config_get_bool(cfg, "Video", "AutoRemux"),
//   config_get_string(cfg, "Output", "Mode") ("Simple" or "Advanced"),
//   config_get_string(cfg, "AdvOut", "RecType"),
//   config_get_string(cfg, simple ? "SimpleOutput" : "AdvOut", "RecFormat2").
//
// FileMover callbacks arrive on its worker thread and hop back with
// run_on_ui() before logging or calling the ClipSorted callback.
//
// UI thread only.
#pragma once

#include "file-mover.hpp"
#include "game-state.hpp"
#include "settings-store.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace clip_atlas {

enum class ClipKind {
	replay,
	recording,  // M2
	screenshot, // M2
};

struct ClipSorted {
	ClipKind kind = ClipKind::replay;
	MoveStatus status = MoveStatus::failed;
	std::filesystem::path original_path;
	std::filesystem::path final_path; // empty unless moved / copied_source_kept
	std::string game_key;             // ResolvedGame::folder_key; "" for unsorted
	std::string game_name;            // ResolvedGame::display_name; "" for unsorted
	std::string error;
};

class FrontendEvents {
public:
	// Called on the UI thread once per file (a remuxed pair gives two calls).
	using ClipSortedCallback = std::function<void(const ClipSorted &)>;

	FrontendEvents(const GameState &state, SettingsStore &settings, FileMover &mover);
	~FrontendEvents(); // calls stop()

	FrontendEvents(const FrontendEvents &) = delete;
	FrontendEvents &operator=(const FrontendEvents &) = delete;

	// obs_frontend_add_event_callback / obs_frontend_remove_event_callback.
	void start();
	void stop(); // idempotent; results of jobs still in flight are dropped

	void set_clip_sorted_callback(ClipSortedCallback callback);

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace clip_atlas
