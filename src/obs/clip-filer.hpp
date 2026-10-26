// Clip Atlas — files saved clips into game folders and reports each file once.
//
// FrontendEvents (replays, screenshots) and RecordingEvents (recordings) decide
// which files a save produced and which game they belong to; ClipFiler does the
// rest, the same way for every kind:
//   1. dest = destination_dir(path_from_utf8(SettingsStore::effective_clips_folder()),
//      game.name, year_month_of(system_clock::now()),
//      {settings.game_folders, settings.monthly_folders, settings.unsorted_folder}),
//      computed when file() is called. With game folders off that is the
//      clips folder itself.
//   2. For each file, in order, on this filer's own `waits` FileMover (so a long
//      wait never delays a move on the plugin's mover):
//        - remuxed empty: wait_stable(file, 1 s, 30 s); stable or timed out
//          both move the file (OBS has finished writing it by then).
//        - remuxed set: OBS reads the original while it writes the remuxed
//          copy, so wait_stable(remuxed, 3 s, remux_timeout(kind)). Stable:
//          move the original, then the copy. Timed out: log a warning and
//          move only the original.
//        - cancelled (the filer is stopping): nothing moves.
//      Each wait result hops to the UI thread with run_on_ui() before it calls
//      FileMover::move() on the plugin's mover.
//   3. Each move result hops to the UI thread, is logged ("<kind> sorted to
//      <path>" at INFO for moved, a copied-original-kept line at INFO, or
//      "<kind> not sorted (<reason>); the clip was left at <path>" at
//      WARNING for failed and cancelled), then goes to the ClipSorted
//      callback once per file. Nothing is logged or reported after stop().
//
// Deferred work captures a std::weak_ptr to the filer's shared state, never
// `this`; the weak_ptr is locked and released only on the UI thread, so the
// waits mover is never destroyed on its own worker thread. (This is how M1's
// frontend-events.cpp already works; ClipFiler is that code made generic.)
//
// UI thread only (all public functions).
#pragma once

#include "clip-routing.hpp"
#include "activity-model.hpp"
#include "file-mover.hpp"
#include "remux-rules.hpp"
#include "settings-store.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace clip_atlas {

// One filed file, as reported to the dock (M3) and the save sound.
struct ClipSorted {
	ClipKind kind = ClipKind::replay;
	MoveStatus status = MoveStatus::failed;
	std::filesystem::path original_path;
	std::filesystem::path final_path; // empty unless moved / copied_source_kept
	std::string game_key;             // ClipGame::key; "" for unsorted
	std::string game_name;            // ClipGame::name; "" for unsorted
	std::string error;
	// The same for every file of one save (a replay and its remuxed copy, or
	// every part of a split recording and their copies), and different for
	// every save across all filers in this OBS session. Lets a listener act
	// once per save, e.g. play one sound for a remuxed replay.
	std::uint64_t save_id = 0;
};

using ClipSortedCallback = std::function<void(const ClipSorted &)>;
using ActivityCallback = std::function<void(const ActivityEvent &)>;

// One file a save produced.
struct ClipFile {
	std::filesystem::path file;
	// expected_remux_path(file, read_remux_config()); empty when OBS will not
	// remux this file.
	std::filesystem::path remuxed;
};

// How long to wait for OBS's remuxed copy to settle: 10 minutes for replays
// and screenshots, 60 minutes for recordings (a long recording on a slow disk
// can take more than 10 minutes to remux).
std::chrono::milliseconds remux_timeout(ClipKind kind);

// The profile settings that decide whether OBS auto-remuxes, read from
// obs_frontend_get_profile_config() (config_t *, owned by OBS, not freed):
//   auto_remux       = config_get_bool(cfg, "Video", "AutoRemux")
//   simple_mode      = Mode == NULL || strcmp(Mode, "Simple") == 0, with
//                      Mode = config_get_string(cfg, "Output", "Mode")
//   advanced_ffmpeg  = RecType = config_get_string(cfg, "AdvOut", "RecType")
//                      is non-NULL and equals "FFmpeg" ignoring ASCII case
//   recording_format = config_get_string(cfg, simple_mode ? "SimpleOutput" : "AdvOut", "RecFormat2"), "" if NULL
// This matches OBSBasic::AutoRemux in OBS 31.0. A NULL profile gives
// RemuxConfig{} (no remux). UI thread.
RemuxConfig read_remux_config();

class ClipFiler {
public:
	ClipFiler(SettingsStore &settings, FileMover &mover);
	// Calls stop().
	~ClipFiler();

	ClipFiler(const ClipFiler &) = delete;
	ClipFiler &operator=(const ClipFiler &) = delete;

	// Files one save. Every file gets the same new save_id. Returns false,
	// logs a warning naming every path ("no clips folder is known ...; it was
	// left at ..."), and queues nothing when effective_clips_folder() is
	// empty, `files` is empty, or the filer has stopped (then silently). The
	// caller has already checked sorting_enabled(). Never throws.
	bool file(ClipKind kind, const std::vector<ClipFile> &files, const ClipGame &game);

	// Idempotent. Shuts the waits mover down with shutdown(false) (cancelled
	// waits move nothing) and drops every later result. A file() after stop()
	// does nothing.
	void stop();

	void set_clip_sorted_callback(ClipSortedCallback callback);
	// Observational only, delivered on the UI thread, never after stop().
	void set_activity_callback(ActivityCallback callback);

private:
	struct Shared;
	std::shared_ptr<Shared> shared_;
};

} // namespace clip_atlas
