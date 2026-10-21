// Clip Atlas — the General page of the settings dialog. settings-dialog.hpp says what the dialog shows; this file
// says how it talks to the store.
//
// OK replaces only the fields this page shows, so the fields it doesn't show (detection, games, ...) keep the values
// the store has. Cancel and Esc change nothing.
#include "settings-dialog.hpp"

#include "settings-model.hpp"
#include "settings-store.hpp"
#include "sound-player.hpp"
#include "ui-thread.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>

#include <exception>
#include <string>

namespace clip_atlas {

namespace {

// Settings and OBS strings are UTF-8 std::strings, while QString is UTF-16. Each crossing goes through one of these
// helpers, so none of them falls back to the system code page.
QString to_qstring(const std::string &utf8)
{
	return QString::fromUtf8(utf8.c_str());
}

std::string to_utf8(const QString &text)
{
	return text.toUtf8().toStdString();
}

QString module_text(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

// The volume label, "80 %". The suffix comes from the locale, so the width measured for the label is the width of the
// text it shows.
QString percent_text(int percent)
{
	return QString::number(percent) + module_text("Settings.PercentSuffix");
}

// The dialog that is open, if any. Qt deletes it when it closes (WA_DeleteOnClose), which resets this pointer.
QPointer<SettingsDialog> open_dialog;

} // namespace

SettingsDialog::SettingsDialog(SettingsStore &store, QWidget *parent) : QDialog(parent), store_(store)
{
	setWindowTitle(module_text("Settings.Title"));

	auto *clips_box = new QGroupBox(module_text("Settings.Clips"), this);
	auto *clips_form = new QFormLayout(clips_box);

	clips_folder_ = new QLineEdit(this);
	// Room for a typical folder path. Measured in the widget's font, so it scales with the theme.
	clips_folder_->setMinimumWidth(clips_folder_->fontMetrics().horizontalAdvance(QLatin1Char('0')) * 40);
	auto *browse = new QPushButton(module_text("Settings.Browse"), this);
	// Enter in the folder field should press OK, so Browse must not take the default-button role.
	browse->setAutoDefault(false);
	connect(browse, &QPushButton::clicked, this, [this] { browse_clips_folder(); });
	auto *folder_row = new QHBoxLayout;
	folder_row->addWidget(clips_folder_, 1);
	folder_row->addWidget(browse);
	clips_form->addRow(module_text("Settings.ClipsFolder"), folder_row);

	monthly_folders_ = new QCheckBox(module_text("Settings.MonthlyFolders"), this);
	clips_form->addRow(monthly_folders_);

	unsorted_folder_ = new QLineEdit(this);
	clips_form->addRow(module_text("Settings.UnsortedFolder"), unsorted_folder_);

	// What to sort (M2). The widgets are created in the order they appear, so Tab moves down the page.
	auto *sort_box = new QGroupBox(module_text("Settings.WhatToSort"), this);
	auto *sort_form = new QFormLayout(sort_box);

	sort_replays_ = new QCheckBox(module_text("Settings.SortReplays"), this);
	sort_form->addRow(sort_replays_);

	sort_recordings_ = new QCheckBox(module_text("Settings.SortRecordings"), this);
	sort_form->addRow(sort_recordings_);

	sort_screenshots_ = new QCheckBox(module_text("Settings.SortScreenshots"), this);
	sort_form->addRow(sort_screenshots_);

	recording_dominant_game_ = new QCheckBox(module_text("Settings.RecordingDominantGame"), this);
	sort_form->addRow(recording_dominant_game_);
	// Disabled rather than hidden, so the choice stays visible. Its checked value is kept and saved either way.
	connect(sort_recordings_, &QCheckBox::toggled, this,
		[this](bool on) { recording_dominant_game_->setEnabled(on); });

	auto *replay_box = new QGroupBox(module_text("Settings.ReplayBuffer"), this);
	auto *replay_form = new QFormLayout(replay_box);

	replay_auto_start_ = new QCheckBox(module_text("Settings.ReplayAutoStart"), this);
	replay_form->addRow(replay_auto_start_);

	replay_auto_stop_ = new QCheckBox(module_text("Settings.ReplayAutoStop"), this);
	replay_form->addRow(replay_auto_stop_);

	grace_period_ = new QSpinBox(this);
	grace_period_->setRange(Settings::kMinGracePeriodSeconds, Settings::kMaxGracePeriodSeconds);
	grace_period_->setSuffix(module_text("Settings.SecondsSuffix"));
	replay_form->addRow(module_text("Settings.GracePeriod"), grace_period_);

	// Save sound (M2): an optional .wav played once a replay is saved. The file, volume and Test rows sit in one
	// container, so a single setEnabled() greys their labels as well as their fields.
	auto *sound_box = new QGroupBox(module_text("Settings.SaveSound"), this);
	auto *sound_form = new QFormLayout(sound_box);

	play_save_sound_ = new QCheckBox(module_text("Settings.PlaySaveSound"), this);
	sound_form->addRow(play_save_sound_);

	auto *sound_details = new QWidget(this);
	auto *details_form = new QFormLayout(sound_details);
	details_form->setContentsMargins(0, 0, 0, 0);
	sound_form->addRow(sound_details);

	save_sound_path_ = new QLineEdit(sound_details);
	// Same width as the clips folder field.
	save_sound_path_->setMinimumWidth(save_sound_path_->fontMetrics().horizontalAdvance(QLatin1Char('0')) * 40);
	auto *browse_sound = new QPushButton(module_text("Settings.Browse"), sound_details);
	// As for the clips folder, Enter should press OK rather than Browse.
	browse_sound->setAutoDefault(false);
	connect(browse_sound, &QPushButton::clicked, this, [this] { browse_save_sound(); });
	auto *file_row = new QHBoxLayout;
	file_row->addWidget(save_sound_path_, 1);
	file_row->addWidget(browse_sound);
	details_form->addRow(module_text("Settings.SaveSoundFile"), file_row);

	save_sound_volume_ = new QSlider(Qt::Horizontal, sound_details);
	save_sound_volume_->setRange(Settings::kMinSaveSoundVolume, Settings::kMaxSaveSoundVolume);
	auto *volume_label = new QLabel(sound_details);
	// Sized for the widest value, so the row does not shift as the slider moves.
	volume_label->setMinimumWidth(
		volume_label->fontMetrics().horizontalAdvance(percent_text(Settings::kMaxSaveSoundVolume)));
	volume_label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	volume_label->setText(percent_text(save_sound_volume_->value()));
	// load() moves the slider through this signal too, so the label follows the stored volume.
	connect(save_sound_volume_, &QSlider::valueChanged, this,
		[volume_label](int value) { volume_label->setText(percent_text(value)); });
	auto *volume_row = new QHBoxLayout;
	volume_row->addWidget(save_sound_volume_, 1);
	volume_row->addWidget(volume_label);
	details_form->addRow(module_text("Settings.SaveSoundVolume"), volume_row);

	auto *test_sound = new QPushButton(module_text("Settings.SaveSoundTest"), sound_details);
	test_sound->setAutoDefault(false);
	connect(test_sound, &QPushButton::clicked, this, [this] { test_save_sound(); });
	// Kept at its natural width: a bare button would stretch across the field column.
	auto *test_row = new QHBoxLayout;
	test_row->addWidget(test_sound);
	test_row->addStretch(1);
	details_form->addRow(QString(), test_row);

	// The help text stays enabled: it says what the sound does whether or not it is on.
	auto *sound_help = new QLabel(module_text("Settings.SaveSoundHelp"), this);
	sound_help->setWordWrap(true);
	sound_form->addRow(sound_help);

	connect(play_save_sound_, &QCheckBox::toggled, this,
		[sound_details](bool on) { sound_details->setEnabled(on); });

	// Updates (M5): the daily check's switch, and a line saying nothing is installed without asking. The check
	// reads the setting when each check is due, so no signal is needed here.
	auto *updates_box = new QGroupBox(module_text("Settings.Updates"), this);
	auto *updates_form = new QFormLayout(updates_box);

	check_for_updates_ = new QCheckBox(module_text("Settings.CheckForUpdates"), this);
	updates_form->addRow(check_for_updates_);

	// Plain text: the help line is a sentence, not markup.
	auto *updates_help = new QLabel(module_text("Settings.CheckForUpdates.Help"), this);
	updates_help->setTextFormat(Qt::PlainText);
	updates_help->setWordWrap(true);
	updates_form->addRow(updates_help);

	// The standard roles keep Enter and Esc working. The labels come from the locale file like every other string,
	// so one translation covers the whole dialog.
	buttons_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	if (QPushButton *ok = buttons_->button(QDialogButtonBox::Ok))
		ok->setText(module_text("Settings.OK"));
	if (QPushButton *cancel = buttons_->button(QDialogButtonBox::Cancel))
		cancel->setText(module_text("Settings.Cancel"));
	// The lambdas call the virtual accept(). Passing &QDialog::accept here would call the base
	// version and skip the save.
	connect(buttons_, &QDialogButtonBox::accepted, this, [this] { accept(); });
	connect(buttons_, &QDialogButtonBox::rejected, this, [this] { reject(); });

	auto *root = new QVBoxLayout(this);
	root->addWidget(clips_box);
	root->addWidget(sort_box);
	root->addWidget(replay_box);
	root->addWidget(sound_box);
	root->addWidget(updates_box);
	root->addWidget(buttons_);

	load(store_.get());
	// toggled fires only when a box changes, so the enabled states are set from the loaded values here.
	recording_dominant_game_->setEnabled(sort_recordings_->isChecked());
	sound_details->setEnabled(play_save_sound_->isChecked());
}

void SettingsDialog::accept()
{
	try {
		Settings settings = store_.get();
		settings.clips_folder = to_utf8(clips_folder_->text().trimmed());
		settings.monthly_folders = monthly_folders_->isChecked();
		// Not trimmed here: validated() runs sanitize_folder_name(), which trims it and turns an empty
		// name into "Unsorted".
		settings.unsorted_folder = to_utf8(unsorted_folder_->text());
		settings.replay_auto_start = replay_auto_start_->isChecked();
		settings.replay_auto_stop = replay_auto_stop_->isChecked();
		settings.grace_period_seconds = grace_period_->value();
		settings.sort_replays = sort_replays_->isChecked();
		settings.sort_recordings = sort_recordings_->isChecked();
		settings.sort_screenshots = sort_screenshots_->isChecked();
		// Saved even while the box is disabled (Recordings off), so turning Recordings back on restores it.
		settings.recording_dominant_game = recording_dominant_game_->isChecked();
		settings.play_save_sound = play_save_sound_->isChecked();
		settings.save_sound_path = to_utf8(save_sound_path_->text().trimmed());
		settings.save_sound_volume = save_sound_volume_->value();
		settings.check_for_updates = check_for_updates_->isChecked();
		// set() applies the settings for this session even if the file can't be written, so the
		// dialog closes either way. The warning is the only sign that the change will not survive a
		// restart.
		if (!store_.set(settings))
			QMessageBox::warning(this, module_text("Settings.Title"), module_text("Settings.SaveFailed"));
	} catch (const std::exception &e) {
		// Nothing was applied, so the dialog stays open and what the user typed is still there.
		obs_log(LOG_ERROR, "could not apply the Clip Atlas settings: %s", e.what());
		return;
	}
	QDialog::accept();
}

void SettingsDialog::browse_clips_folder()
{
	try {
		// The picker opens where the user already is: the typed folder, or the folder an empty field means.
		const QString typed = clips_folder_->text().trimmed();
		const QString start = typed.isEmpty() ? to_qstring(store_.effective_clips_folder()) : typed;
		const QString chosen =
			QFileDialog::getExistingDirectory(this, module_text("Settings.ClipsFolder.BrowseTitle"), start);
		// Cancel returns an empty string, which must not clear the field.
		if (!chosen.isEmpty())
			clips_folder_->setText(QDir::toNativeSeparators(chosen));
	} catch (const std::exception &e) {
		obs_log(LOG_ERROR, "could not browse for the clips folder: %s", e.what());
	}
}

void SettingsDialog::browse_save_sound()
{
	try {
		// The picker opens in the folder of the file already typed, if there is one.
		const QString typed = save_sound_path_->text().trimmed();
		const QString start = typed.isEmpty() ? QString() : QFileInfo(typed).absolutePath();
		const QString chosen =
			QFileDialog::getOpenFileName(this, module_text("Settings.SaveSoundFile.BrowseTitle"), start,
						     module_text("Settings.SaveSoundFile.Filter"));
		// Cancel returns an empty string, which must not clear the field.
		if (!chosen.isEmpty())
			save_sound_path_->setText(QDir::toNativeSeparators(chosen));
	} catch (const std::exception &e) {
		obs_log(LOG_ERROR, "could not browse for the sound file: %s", e.what());
	}
}

void SettingsDialog::test_save_sound()
{
	try {
		const QString path = save_sound_path_->text().trimmed();
		if (path.isEmpty()) {
			QMessageBox::information(this, module_text("Settings.Title"),
						 module_text("Settings.SaveSoundNoFile"));
			return;
		}
		// Plays the file and volume shown here, so they can be tried before OK. The player may report from its
		// own thread, so the report only hops to the UI thread. There the dialog is checked again, since it may
		// have closed in the meantime.
		QPointer<SettingsDialog> dialog(this);
		sound_player().play(to_utf8(path), save_sound_volume_->value(), [dialog](const std::string &error) {
			run_on_ui([dialog, error] {
				if (dialog.isNull() || error.empty())
					return;
				QMessageBox::warning(dialog.data(), module_text("Settings.Title"),
						     module_text("Settings.SaveSoundTestFailed") + QLatin1Char('\n') +
							     to_qstring(error));
			});
		});
	} catch (const std::exception &e) {
		obs_log(LOG_ERROR, "could not play the Clip Atlas save sound: %s", e.what());
	}
}

void SettingsDialog::load(const Settings &settings)
{
	clips_folder_->setText(to_qstring(settings.clips_folder));
	// An empty field means OBS's recording folder, so the placeholder names that folder. A stored folder fills the
	// field, and once the user clears it the store would still report that stored folder, so the placeholder falls
	// back to the words instead.
	QString placeholder = module_text("Settings.ClipsFolder.Default");
	if (settings.clips_folder.empty()) {
		const std::string recording = store_.effective_clips_folder();
		if (!recording.empty())
			placeholder = to_qstring(recording);
	}
	clips_folder_->setPlaceholderText(placeholder);

	monthly_folders_->setChecked(settings.monthly_folders);
	unsorted_folder_->setText(to_qstring(settings.unsorted_folder));
	replay_auto_start_->setChecked(settings.replay_auto_start);
	replay_auto_stop_->setChecked(settings.replay_auto_stop);
	grace_period_->setValue(settings.grace_period_seconds);
	sort_replays_->setChecked(settings.sort_replays);
	sort_recordings_->setChecked(settings.sort_recordings);
	sort_screenshots_->setChecked(settings.sort_screenshots);
	recording_dominant_game_->setChecked(settings.recording_dominant_game);
	play_save_sound_->setChecked(settings.play_save_sound);
	save_sound_path_->setText(to_qstring(settings.save_sound_path));
	save_sound_volume_->setValue(settings.save_sound_volume);
	check_for_updates_->setChecked(settings.check_for_updates);
}

void open_settings_dialog(SettingsStore &store, QWidget *parent)
{
	try {
		// A closed dialog waits to be deleted and is hidden, so it doesn't count as open. Re-showing
		// it would bring back a window that is about to go.
		if (!open_dialog.isNull() && open_dialog->isVisible()) {
			open_dialog->show();
			open_dialog->raise();
			open_dialog->activateWindow();
			return;
		}
		open_dialog = new SettingsDialog(store, parent);
		open_dialog->setAttribute(Qt::WA_DeleteOnClose);
		open_dialog->show();
	} catch (const std::exception &e) {
		obs_log(LOG_ERROR, "could not open the Clip Atlas settings: %s", e.what());
	}
}

void close_settings_dialog()
{
	// Deleted now, not with deleteLater(): the event loop may not run again before the plugin unloads.
	delete open_dialog.data();
}

} // namespace clip_atlas
