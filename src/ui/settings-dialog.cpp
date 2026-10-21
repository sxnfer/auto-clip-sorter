// Clip Atlas — the General page of the settings dialog. settings-dialog.hpp says what the dialog shows; this file
// says how it talks to the store.
//
// OK replaces only the fields this page shows, so the fields it doesn't show (detection, games, ...) keep the values
// the store has. Cancel and Esc change nothing.
#include "settings-dialog.hpp"

#include "settings-model.hpp"
#include "settings-store.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFontMetrics>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
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
	root->addWidget(replay_box);
	root->addWidget(buttons_);

	load(store_.get());
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
}

void open_settings_dialog(SettingsStore &store, QWidget *parent)
{
	try {
		// A closed dialog waits to be deleted and is hidden, so it doesn't count as open. Re-showing it would bring
		// back a window that is about to go.
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
