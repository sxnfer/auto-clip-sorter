// Clip Atlas — the settings dialog, reached from Tools → "Clip Atlas Settings…".
//
// M1 has one page, General: clips folder, monthly folders, unsorted folder
// name, and the Replay Buffer auto start/stop toggles and grace period. M4
// turns this into OBS-style sidebar pages (Detection, Replay Buffer, Games,
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
	void load(const Settings &settings);

	SettingsStore &store_;
	QLineEdit *clips_folder_ = nullptr;
	QCheckBox *monthly_folders_ = nullptr;
	QLineEdit *unsorted_folder_ = nullptr;
	QCheckBox *replay_auto_start_ = nullptr;
	QCheckBox *replay_auto_stop_ = nullptr;
	QSpinBox *grace_period_ = nullptr;
	QDialogButtonBox *buttons_ = nullptr;
};

// Shows the dialog, creating it on first use. A second call while it is open
// raises the existing window instead of opening another. The dialog is
// non-modal, deletes itself on close, and is parented to `parent` (OBS's main
// window, from obs_frontend_get_main_window()).
void open_settings_dialog(SettingsStore &store, QWidget *parent);

} // namespace clip_atlas
