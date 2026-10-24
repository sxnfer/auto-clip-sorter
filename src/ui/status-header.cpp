// Clip Atlas — the top of the dock. status-header.hpp shows the three layouts. The widgets are made once; a layout
// change only arranges them again, so the focus and the switch's state stay put.
#include "status-header.hpp"

#include "dock-controls.hpp"
#include "dock-style.hpp"

#include <obs-module.h>

#include <QEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSize>
#include <QVBoxLayout>

#include <utility>

namespace clip_atlas {

namespace {

// The save icon's size next to the button's text.
constexpr int kSaveIconPx = 16;

QString module_text(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

bool replay_running(ReplayBufferStatus status)
{
	return status == ReplayBufferStatus::active_by_us || status == ReplayBufferStatus::active_by_user;
}

} // namespace

StatusHeader::StatusHeader(QWidget *parent) : QWidget(parent)
{
	avatar_ = new GameAvatar(this);
	title_ = new GameTitle(this);
	pill_ = new StatusPill(this);
	pill_->setToolTip(module_text("Dock.Replay.Tooltip"));

	auto_sort_ = new ToggleSwitch(module_text("Dock.AutoSort"), this);
	auto_sort_->setToolTip(module_text("Dock.AutoSort.Tooltip"));
	// clicked(), not toggled(): set_auto_sort() changes the switch from code, and that must not call the callback back.
	connect(auto_sort_, &QAbstractButton::clicked, this, [this](bool on) {
		// Copied before the call, because the callback may replace itself.
		const AutoSortCallback callback = auto_sort_callback_;
		if (callback)
			callback(on);
	});

	save_ = new QPushButton(this);
	save_->setIcon(dock_icon(DockIcon::save, palette(), kSaveIconPx));
	save_->setIconSize(QSize(kSaveIconPx, kSaveIconPx));
	save_->setAccessibleName(module_text("Dock.SaveReplay"));
	connect(save_, &QPushButton::clicked, this, [this] {
		const SaveCallback callback = save_callback_;
		if (callback)
			callback();
	});

	rebuild_layout();
	update_title();
	update_pill();
	update_save_button();
}

void StatusHeader::set_layout(Layout layout)
{
	if (layout == layout_)
		return;
	layout_ = layout;
	rebuild_layout();
	update_save_button();
}

void StatusHeader::set_game(const QString &name)
{
	game_name_ = name;
	update_title();
}

void StatusHeader::set_replay_status(ReplayBufferStatus status)
{
	replay_status_ = status;
	update_pill();
	// The status's width decides whether Save replay keeps its text.
	update_save_button();
}

void StatusHeader::set_replay_auto_start(bool on)
{
	replay_auto_start_ = on;
	update_pill();
}

void StatusHeader::set_auto_sort(bool on)
{
	// setChecked() doesn't emit clicked(), but blocking the switch's signals makes "never calls back" hold without
	// depending on that. The knob still moves, because the switch paints it from its check state.
	const QSignalBlocker blocker(auto_sort_);
	auto_sort_->setChecked(on);
	auto_sort_->update();
}

void StatusHeader::set_auto_sort_callback(AutoSortCallback callback)
{
	auto_sort_callback_ = std::move(callback);
}

void StatusHeader::set_save_callback(SaveCallback callback)
{
	save_callback_ = std::move(callback);
}

void StatusHeader::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	update_save_button();
}

void StatusHeader::changeEvent(QEvent *event)
{
	QWidget::changeEvent(event);
	// The base class can send events before the children exist.
	if (!save_ || !pill_)
		return;
	switch (event->type()) {
	case QEvent::PaletteChange:
	case QEvent::StyleChange:
		// The icon's pixmaps hold the old text colour, and the pill's dot the old accents.
		save_->setIcon(dock_icon(DockIcon::save, palette(), kSaveIconPx));
		update_pill();
		break;
	case QEvent::FontChange:
		update_save_button();
		break;
	default:
		break;
	}
}

void StatusHeader::rebuild_layout()
{
	// The old layout goes, with its nested layouts; the widgets stay, children of the header.
	delete layout();

	auto *identity = new QHBoxLayout;
	identity->setSpacing(10);
	identity->addWidget(avatar_, 0, Qt::AlignVCenter);
	identity->addWidget(title_, 1, Qt::AlignVCenter);

	switch (layout_) {
	case Layout::column: {
		identity->addSpacing(6);
		identity->addWidget(auto_sort_, 0, Qt::AlignTop);
		auto *status = new QHBoxLayout;
		status->setSpacing(8);
		status->addWidget(pill_, 1);
		status->addWidget(save_);
		auto *root = new QVBoxLayout(this);
		root->setContentsMargins(10, 10, 10, 8);
		root->setSpacing(10);
		root->addLayout(identity);
		root->addLayout(status);
		break;
	}
	case Layout::rail: {
		auto *controls = new QHBoxLayout;
		controls->setSpacing(8);
		controls->addWidget(auto_sort_);
		controls->addStretch(1);
		controls->addWidget(save_);
		auto *root = new QVBoxLayout(this);
		root->setContentsMargins(10, 10, 10, 6);
		root->setSpacing(10);
		root->addLayout(identity);
		root->addWidget(pill_);
		root->addLayout(controls);
		break;
	}
	case Layout::wide: {
		auto *root = new QHBoxLayout(this);
		root->setContentsMargins(12, 10, 12, 10);
		root->setSpacing(14);
		root->addLayout(identity, 1);
		root->addWidget(pill_, 0, Qt::AlignVCenter);
		root->addWidget(auto_sort_, 0, Qt::AlignVCenter);
		root->addWidget(save_, 0, Qt::AlignVCenter);
		break;
	}
	}
}

void StatusHeader::update_title()
{
	const bool no_game = game_name_.isEmpty();
	avatar_->set_game(game_name_);
	if (no_game)
		title_->set_texts(module_text("Dock.WaitingForGame"), module_text("Dock.NoGame"), true);
	else
		title_->set_texts(module_text("Dock.NowPlaying"), game_name_, false);
}

void StatusHeader::update_pill()
{
	const ThemeAccents &accents = theme_accents();
	switch (replay_status_) {
	case ReplayBufferStatus::active_by_us:
		pill_->set_status(accents.success, module_text("Dock.Replay.On"), module_text("Dock.Replay.ByUs"));
		break;
	case ReplayBufferStatus::active_by_user:
		pill_->set_status(accents.success, module_text("Dock.Replay.On"), QString());
		break;
	case ReplayBufferStatus::inactive:
		// A stopped buffer is expected between games, so it reads as a hint, not an alarm.
		pill_->set_status(muted_text(palette()), module_text("Dock.Replay.Off"),
				  replay_auto_start_ ? module_text("Dock.Replay.WithGame") : QString());
		break;
	case ReplayBufferStatus::disabled_in_obs:
		pill_->set_status(accents.warning, module_text("Dock.Replay.Disabled"), QString());
		break;
	}
}

void StatusHeader::update_save_button()
{
	const QString label = module_text("Dock.SaveReplay");
	// The button's full width: the theme's padding is unknown here, so it is measured from the text and the icon with
	// room for a typical padding.
	const QFontMetrics metrics(save_->font());
	const int full_width = metrics.horizontalAdvance(label) + kSaveIconPx + 36;
	const int margins = layout_ == Layout::wide ? 24 : 20;
	int beside = 0; // what shares the button's row
	switch (layout_) {
	case Layout::column:
		// The pill keeps room for its status; the detail can go.
		beside = pill_->status_width() + 8;
		break;
	case Layout::rail:
		beside = auto_sort_->sizeHint().width() + 8;
		break;
	case Layout::wide:
		beside = 0;
		break;
	}
	const bool compact = layout_ != Layout::wide && width() - margins - beside < full_width;
	const QString shown = compact ? QString() : label;
	if (save_->text() != shown)
		save_->setText(shown);
	save_->setToolTip(compact ? label + QStringLiteral("\n") + module_text("Dock.SaveReplay.Tooltip")
				  : module_text("Dock.SaveReplay.Tooltip"));
	save_->setEnabled(replay_running(replay_status_));
}

} // namespace clip_atlas
