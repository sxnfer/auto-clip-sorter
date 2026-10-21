// Clip Atlas — the top of the dock. Colours come from the palette, and the game name is elided here, because a
// label that asks for its full width would widen the whole dock.
#include "status-header.hpp"

#include <obs-module.h>

#include <QCheckBox>
#include <QChar>
#include <QColor>
#include <QEvent>
#include <QFont>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QPalette>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QVBoxLayout>

#include <utility>

namespace clip_atlas {

namespace {

QString module_text(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

bool replay_running(ReplayBufferStatus status)
{
	return status == ReplayBufferStatus::active_by_us || status == ReplayBufferStatus::active_by_user;
}

const char *replay_key(ReplayBufferStatus status)
{
	switch (status) {
	case ReplayBufferStatus::disabled_in_obs:
		return "Dock.Replay.Disabled";
	case ReplayBufferStatus::inactive:
		return "Dock.Replay.Off";
	case ReplayBufferStatus::active_by_us:
		return "Dock.Replay.OnByUs";
	case ReplayBufferStatus::active_by_user:
		return "Dock.Replay.On";
	}
	return "Dock.Replay.Off";
}

} // namespace

StatusHeader::StatusHeader(QWidget *parent) : QWidget(parent)
{
	game_ = new QLabel(this);
	game_->setTextFormat(Qt::PlainText);
	// Ignored: the label must not ask for room for the full name. It gets whatever width is left and elides to it.
	QSizePolicy game_policy = game_->sizePolicy();
	game_policy.setHorizontalPolicy(QSizePolicy::Ignored);
	game_->setSizePolicy(game_policy);
	apply_game_font();

	replay_ = new QLabel(this);
	replay_->setTextFormat(Qt::RichText);
	// Wraps rather than forcing the dock wider when the window is narrow.
	replay_->setWordWrap(true);

	auto_sort_ = new QCheckBox(module_text("Dock.AutoSort"), this);
	auto_sort_->setToolTip(module_text("Dock.AutoSort.Tooltip"));
	// clicked(), not toggled(): set_auto_sort() changes the box from code, and that must not call the callback back.
	connect(auto_sort_, &QCheckBox::clicked, this, [this](bool on) {
		// Copied before the call, because the callback may replace itself.
		const AutoSortCallback callback = auto_sort_callback_;
		if (callback)
			callback(on);
	});

	auto *replay_row = new QHBoxLayout;
	replay_row->addWidget(replay_);
	replay_row->addStretch(1);
	replay_row->addWidget(auto_sort_);

	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(4, 4, 4, 4);
	root->setSpacing(2);
	root->addWidget(game_);
	root->addLayout(replay_row);

	update_game_label();
	update_replay_label();
}

void StatusHeader::set_game(const QString &name)
{
	game_name_ = name;
	update_game_label();
}

void StatusHeader::set_replay_status(ReplayBufferStatus status)
{
	replay_status_ = status;
	update_replay_label();
}

void StatusHeader::set_auto_sort(bool on)
{
	// setChecked() doesn't emit clicked(), but blocking the box's signals makes "never calls back" hold without
	// depending on that.
	const QSignalBlocker blocker(auto_sort_);
	auto_sort_->setChecked(on);
}

void StatusHeader::set_auto_sort_callback(AutoSortCallback callback)
{
	auto_sort_callback_ = std::move(callback);
}

void StatusHeader::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	update_game_label();
}

void StatusHeader::changeEvent(QEvent *event)
{
	QWidget::changeEvent(event);
	// The base class can send events before the children exist.
	if (!game_ || !replay_)
		return;
	switch (event->type()) {
	case QEvent::FontChange:
		// The label has its own font, so it doesn't inherit a change. Re-derived here, and the elision follows it.
		apply_game_font();
		update_game_label();
		break;
	case QEvent::PaletteChange:
	case QEvent::StyleChange:
		update_game_label();
		update_replay_label();
		break;
	default:
		break;
	}
}

void StatusHeader::apply_game_font()
{
	QFont game_font = font();
	game_font.setBold(true);
	// pointSizeF() is -1 for a font sized in pixels, and scaling that would give an invalid size.
	if (game_font.pointSizeF() > 0)
		game_font.setPointSizeF(game_font.pointSizeF() * 1.1);
	game_->setFont(game_font);
}

void StatusHeader::update_game_label()
{
	const bool no_game = game_name_.isEmpty();
	const QString full_name = no_game ? module_text("Dock.NoGame") : game_name_;
	// The label has no frame or margins, so its contents rect is the room the text is drawn in.
	const QString shown = game_->fontMetrics().elidedText(full_name, Qt::ElideRight, game_->contentsRect().width());
	if (game_->text() != shown)
		game_->setText(shown);
	game_->setToolTip(no_game ? QString() : full_name);

	if (no_game) {
		// Only the text colour changes. The other roles still follow the theme, because they are not overridden.
		QPalette label_palette = game_->palette();
		label_palette.setColor(QPalette::WindowText, palette().color(QPalette::Disabled, QPalette::WindowText));
		game_->setPalette(label_palette);
	} else {
		// A palette with nothing set means "inherit", which puts the normal text colour back.
		game_->setPalette(QPalette());
	}
}

void StatusHeader::update_replay_label()
{
	// The dot is the only colour in the pill. Running uses the accent colour, and the other states are dimmed so
	// a stopped buffer reads as a hint, not an alarm.
	const QColor dot = replay_running(replay_status_) ? palette().color(QPalette::Highlight)
							  : palette().color(QPalette::Disabled, QPalette::WindowText);
	const QString dot_glyph(QChar(0x25CF)); // U+25CF BLACK CIRCLE
	const QString status = module_text(replay_key(replay_status_)).toHtmlEscaped();
	replay_->setText(QStringLiteral("<span style='color:%1'>%2</span> %3").arg(dot.name(), dot_glyph, status));
	replay_->setToolTip(module_text("Dock.Replay.Tooltip"));
}

} // namespace clip_atlas
