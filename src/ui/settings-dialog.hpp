// Clip Atlas — the settings dialog, reached from Tools → "Clip Atlas Settings…".
//
// One page, General, with these groups, top to bottom:
//   Clips (M1): clips folder + Browse, monthly folders, unsorted folder name.
//   What to sort (M2): Replays, Recordings, Screenshots (Settings::sort_replays,
//     sort_recordings, sort_screenshots), and "File each recording under the
//     game played longest" (recording_dominant_game), enabled only while
//     Recordings is checked.
//   Replay Buffer (M1): auto start, auto stop, grace period.
//   Save sound (M2): "Play a sound when a replay is saved" (play_save_sound);
//     sound file field + Browse (a .wav picker); volume 0-100 %
//     (save_sound_volume); a Test button; and one wrapped line of help text
//     saying Windows plays the sound, so Desktop Audio captures it (it can be
//     heard on stream and in later clips). The file, volume and Test widgets
//     are enabled only while the checkbox is on. Test plays the file and
//     volume shown in the dialog, even before OK, through sound_player(); an
//     empty file field, or a failure the player reports, is shown in a
//     message box.
//   Updates (M5): "Check for updates once a day" (check_for_updates), with one
//     wrapped line of help text (Settings.CheckForUpdates.Help) saying that
//     nothing is downloaded or installed automatically.
// M4 turns this into OBS-style sidebar pages (Detection, Replay Buffer, Games,
// Advanced) without changing how it is opened.
//
// Follows OBS's theme: no hard-coded colours or fonts, standard widgets and
// layouts only. Every string comes from obs_module_text() with its key in
// data/locale/en-US.ini.
//
// UI thread only.
#pragma once

#include <QDialog>

class QCheckBox;
class QDialogButtonBox;
class QLineEdit;
class QSlider;
class QSpinBox;

namespace clip_atlas {

class SettingsStore;
struct Settings;

// No Q_OBJECT: the dialog declares no signals or slots of its own (connections
// use lambdas), and leaving moc out keeps the vtable in settings-dialog.cpp.
class SettingsDialog : public QDialog {
public:
	// Fills the widgets from store.get(). The clips-folder field shows the
	// stored value; when that is empty it stays empty and shows
	// store.effective_clips_folder() as placeholder text, so "use OBS's
	// recording folder" remains the saved choice.
	SettingsDialog(SettingsStore &store, QWidget *parent);

	// OK: builds a Settings from store.get() with the General-page fields
	// replaced, and calls store.set() (which validates, saves and notifies).
	// Fields the dialog doesn't show are kept as they are.
	void accept() override;

private:
	void browse_clips_folder();
	void browse_save_sound();
	void test_save_sound();
	void load(const Settings &settings);

	SettingsStore &store_;
	QLineEdit *clips_folder_ = nullptr;
	QCheckBox *monthly_folders_ = nullptr;
	QLineEdit *unsorted_folder_ = nullptr;
	QCheckBox *sort_replays_ = nullptr;
	QCheckBox *sort_recordings_ = nullptr;
	QCheckBox *sort_screenshots_ = nullptr;
	QCheckBox *recording_dominant_game_ = nullptr;
	QCheckBox *replay_auto_start_ = nullptr;
	QCheckBox *replay_auto_stop_ = nullptr;
	QSpinBox *grace_period_ = nullptr;
	QCheckBox *play_save_sound_ = nullptr;
	QLineEdit *save_sound_path_ = nullptr;
	QSlider *save_sound_volume_ = nullptr;
	QCheckBox *check_for_updates_ = nullptr;
	QDialogButtonBox *buttons_ = nullptr;
};

// Shows the dialog, creating it on first use. A second call while it is open
// raises the existing window instead of opening another. The dialog is
// non-modal, deletes itself on close, and is parented to `parent` (OBS's main
// window, from obs_frontend_get_main_window()).
void open_settings_dialog(SettingsStore &store, QWidget *parent);

// Deletes the dialog now if it is open, without saving. Called from
// obs_module_unload() so no window of ours outlives the plugin's code.
void close_settings_dialog();

} // namespace clip_atlas
