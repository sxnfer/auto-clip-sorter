// Clip Atlas — the clip browser dock (Docks → Clip Atlas).
//
// Layout and behaviour: docs/handover/M3-dock-spec.md. Widget tree, top to
// bottom, in one QVBoxLayout:
//   StatusHeader
//   BannerArea
//   filter row: QComboBox (game filter) + QLineEdit (search, clear button on)
//   QListView (ClipListModel + ClipListDelegate; single selection, uniform
//     row sizes, per-pixel scrolling) or, when there is nothing to show, a
//     centred word-wrapped QLabel (Dock.Empty.NoClips with no clips at all,
//     Dock.Empty.NoMatch when the filter hides every clip); a QStackedWidget
//     switches between them
//   footer: QPushButton Dock.OpenFolder, stretch, QToolButton Refresh
//     (tooltip Dock.Refresh), QToolButton ⚙ (tooltip Dock.Settings)
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
// guess). Other statuses change nothing.
//
// Status: on_game_changed() shows clip_game_for(state.current(),
// settings).name. on_replay_status() updates the pill and the
// replay_buffer_disabled banner: shown while the status is disabled_in_obs and
// settings.replay_auto_start is on (Dock.Banner.ReplayDisabled), hidden
// otherwise. The auto-sort switch shows settings.auto_sort and, when clicked,
// stores the settings with auto_sort changed (SettingsStore::set; the dock is
// updated again through apply_settings()).
//
// Clip actions (right-click menu on a row, in this order; double-click and
// Enter open; Delete recycles; F2 renames):
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
// Footer: Dock.OpenFolder → shell_ops().open_folder(base) (disabled while base
// is empty); Refresh → a new scan; ⚙ → open_settings_dialog(store,
// main window).
//
// Ages: a QTimer calls ClipListModel::refresh_ages() every 60 s while the dock
// is visible.
//
// Lifetime: OBS owns the widget once register_clip_dock() has added it, and
// deletes it with the main window, after OBS_FRONTEND_EVENT_EXIT and before
// obs_module_unload() (OBS 31: OBSBasic owns extraDocks; the main window is
// deleted on close, before obs_shutdown() unloads modules). stop() runs at
// EXIT (stop_all()): it shuts the queue down, stops the timer and the model,
// and makes every public function do nothing. The destructor calls stop() and
// touches nothing outside the widget. GameState, SettingsStore and FileMover
// outlive the dock (destroyed in obs_module_unload()).
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
	void on_activity(const ActivityEvent &event);

	// For the update banner (M5). nullptr after stop().
	BannerArea *banners();

protected:
	void showEvent(QShowEvent *event) override;
	void hideEvent(QHideEvent *event) override;

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
