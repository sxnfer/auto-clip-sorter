// Clip Atlas — the clip browser dock (Docks → Clip Atlas).
//
// Layout and behaviour: docs/handover/M6-dock-spec.md, which amends
// docs/handover/M3-dock-spec.md. The same widgets are arranged three ways,
// by choose_dock_layout() on the dock's size; resizeEvent queues the change,
// and the layout is rebuilt outside it:
//   column (a side dock):  StatusHeader (column), ActivityStatus, BannerArea,
//     the search QLineEdit, ChipBar (2 rows), SegmentBar, the list, a
//     hairline, the footer.
//   row (the short bottom row): a rail on the left with StatusHeader (rail),
//     ActivityStatus and the footer; on the right BannerArea, ChipBar (1 row)
//     beside the search field, and the list as a sideways strip of cards (no
//     headers). The type filter is hidden and reset to All, so no hidden
//     filter is left on.
//   wide (a large, usually floating window): StatusHeader (wide) and
//     ActivityStatus across the top; the filters and the list on the left;
//     ClipPreview of the current clip on the right (entering it selects the
//     first clip when none is); the footer across the bottom.
// ActivityStatus is the filing activity pill (docs/handover/activity-status.md);
// on_activity() feeds it.
// The list is a ClipListView over a ClipListModel; when there is nothing to
// show, a QStackedWidget swaps it for an EmptyState (Dock.Empty.Title and
// .Body with no clips at all, Dock.Empty.NoMatch and .NoMatchHint when the
// filter hides every clip). The footer has the summary (Dock.Summary of the
// index's clip count and size, or Dock.Footer.Truncated when the last scan
// hit the cap) and GhostButtons for Dock.OpenFolder, Dock.Refresh and
// Dock.Settings. A DockToast floats over the bottom of the list (the rail in
// the row layout, below the activity pill, when the toast's text fits there;
// otherwise the strip).
//
// Filters: the chips are Dock.Filter.All with the index's clip count, each
// ClipIndex::games() entry with its count, then Dock.Game.Unfiled when
// has_unfiled(); the choice is kept while its chip exists, otherwise All. The
// segments set ClipQuery::kind (Dock.Kind.All, .Replays, .Recordings,
// .Screenshots). The search sets ClipQuery::search. Ctrl+F focuses the search;
// Escape there clears it, or goes to the list when it is empty.
//
// Scanning (on the dock's own TaskQueue, never on the UI thread):
//   - base = path_from_utf8(SettingsStore::effective_clips_folder()), read on
//     the UI thread when a scan starts; options.replay_prefix =
//     config_get_string(obs_frontend_get_profile_config(), "SimpleOutput",
//     "RecRBPrefix") ("" when NULL or no profile; both output modes use this
//     key in OBS 31 and 32).
//   - scan_clips_folder(base, options) runs on the queue; the result hops back
//     with run_on_ui() and is applied only if its generation is still the
//     newest and the dock is not stopped (a std::weak_ptr to shared state,
//     locked on the UI thread only, guards the dock's lifetime, as in
//     clip-filer.cpp).
//   - A scan starts at start(), on Refresh, from apply_settings() and
//     on_profile_changed() when the base, the replay prefix, the unsorted
//     folder name or monthly folders changed, and on showEvent when the base
//     changed since the last scan (OBS's recording path has no change event).
//   - Banners after each scan: no_clips_folder when base is empty (action
//     Dock.Banner.OpenSettings opens the settings dialog);
//     clips_folder_missing when !base_exists (text Dock.Banner.FolderMissing
//     with %1 = the folder); neither otherwise.
//
// Filing: on_clip_sorted() with status moved or copied_source_kept queues
// describe_clip(base, final_path) on the queue; the entry hops back and is
// upserted with its kind set to clip.kind (the exact type, not the prefix
// guess), marked fresh (ClipListModel::set_fresh()), and the toast says
// Dock.Toast.Filed (moved) or Dock.Toast.Copied (copied_source_kept) with the
// folders below the base (folders_below(), joined by " › "), and the clip's
// title and size; Show reveals it. Status failed shows Dock.Toast.Failed with
// the reason; Show reveals the original. Cancelled changes nothing.
//
// Status: on_game_changed() shows clip_game_for(state.current(),
// settings).name. on_replay_status() updates the pill and Save replay, and
// the replay_buffer_disabled banner: shown while the status is
// disabled_in_obs and settings.replay_auto_start is on
// (Dock.Banner.ReplayDisabled), hidden otherwise. The auto-sort switch shows
// settings.auto_sort and, when clicked, stores the settings with auto_sort
// changed (SettingsStore::set; the dock is updated again through
// apply_settings()). Save replay calls obs_frontend_replay_buffer_save()
// when obs_frontend_replay_buffer_active().
//
// Clip actions (right-click menu on a row or card, and the More action, in
// this order; double-click and Enter open; Delete recycles; F2 renames; the
// hover actions open and reveal; the preview's buttons do each):
//   Dock.Action.Open        shell_ops().open(path)
//   Dock.Action.Reveal      shell_ops().reveal(path)
//   ─────
//   Dock.Action.Rename…     QInputDialog::getText with the stem (file name
//                           without its extension) pre-filled;
//                           renamed_file_name(path, text) → nullopt shows
//                           Dock.Error.BadName and changes nothing; otherwise
//                           shell_ops().rename(path, name); on success the old
//                           row is removed and describe_clip(new path) is
//                           upserted (keeping the old kind).
//   Dock.Action.MoveToGame… QInputDialog::getItem, editable, listing
//                           index().games() plus the current game's name,
//                           minus the clip's own game_folder, sorted; a typed
//                           name is allowed. dest = destination_dir(base, name,
//                           year_month_of(clip.modified), {settings.monthly_folders,
//                           settings.unsorted_folder}); the plugin's FileMover
//                           moves it (mover.move(path, dest, ...), result via
//                           run_on_ui()). moved: remove the old row, upsert
//                           describe_clip(final_path); copied_source_kept:
//                           upsert both; failed/cancelled: Dock.Error.MoveFailed
//                           with the reason. The clip's own folder is refused.
//   ─────
//   Dock.Action.Recycle…    QMessageBox::question(Dock.Confirm.RecycleTitle,
//                           Dock.Confirm.Recycle with %1 = file name), default
//                           No. Yes: shell_ops().recycle(path); "" removes the
//                           row, an error shows Dock.Error.RecycleFailed.
// Errors from Shell calls are shown in a QMessageBox::warning parented to the
// dock and logged at WARNING; nothing else changes. Results that arrive after
// stop() are dropped.
//
// Days: a QTimer calls ClipListModel::refresh_day() every 60 s while the dock
// is visible, so the groups follow midnight.
//
// Theme: on_theme_changed() (OBS_FRONTEND_EVENT_THEME_CHANGED) and the dock's
// own palette and style changes forget the cached theme accents, redraw the
// icons and repaint every child.
//
// Lifetime: OBS owns the widget once register_clip_dock() has added it, and
// deletes it with the main window, after OBS_FRONTEND_EVENT_EXIT and before
// obs_module_unload() (OBS 31: OBSBasic owns extraDocks; the main window is
// deleted on close, before obs_shutdown() unloads modules). stop() runs at
// EXIT (stop_all()): it shuts the queue down, stops the timer, the model, the
// preview and the toast, and makes every public function do nothing. The
// destructor calls stop() and touches nothing outside the widget. GameState,
// SettingsStore and FileMover outlive the dock (destroyed in
// obs_module_unload()).
//
// UI thread only.
#pragma once

#include "banner-area.hpp"
#include "clip-filer.hpp"
#include "file-mover.hpp"
#include "game-state.hpp"
#include "replay-controller.hpp"
#include "settings-store.hpp"

#include <QWidget>

#include <memory>

namespace clip_atlas {

class ClipDock : public QWidget {
public:
	ClipDock(const GameState &state, SettingsStore &settings, FileMover &mover, QWidget *parent = nullptr);
	// Calls stop().
	~ClipDock() override;

	ClipDock(const ClipDock &) = delete;
	ClipDock &operator=(const ClipDock &) = delete;

	// OBS_FRONTEND_EVENT_FINISHED_LOADING: OBS's recording folder can now be
	// read. Shows the current state and starts the first scan. Once only; does
	// nothing after stop().
	void start();
	// OBS_FRONTEND_EVENT_EXIT. Idempotent.
	void stop();

	void apply_settings(const Settings &settings);
	void on_profile_changed();
	void on_game_changed();
	void on_replay_status(ReplayBufferStatus status);
	void on_clip_sorted(const ClipSorted &clip);
	// OBS_FRONTEND_EVENT_THEME_CHANGED.
	void on_theme_changed();
	// A filing step, from ClipFiler on the UI thread, for the activity pill.
	void on_activity(const ActivityEvent &event);

	// For the update banner (M5). nullptr after stop().
	BannerArea *banners();

protected:
	void showEvent(QShowEvent *event) override;
	void hideEvent(QHideEvent *event) override;
	// Queues a layout change when the size calls for one, and moves the toast.
	void resizeEvent(QResizeEvent *event) override;
	// A palette or style change can be a theme change that OBS doesn't announce.
	void changeEvent(QEvent *event) override;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

// Creates the dock and adds it with obs_frontend_add_dock_by_id(
// "clip-atlas-dock", obs_module_text("Dock.Title"), dock). OBS 31 wraps it in
// a floating, hidden OBSDock the first time and restores the user's layout
// afterwards, so register it from obs_module_load() (before OBS restores the
// dock state). Returns nullptr, logs a warning and deletes the widget if OBS
// refuses (the id is already used). OBS owns the widget from then on: never
// delete it.
ClipDock *register_clip_dock(const GameState &state, SettingsStore &settings, FileMover &mover);

} // namespace clip_atlas
