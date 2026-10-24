// Clip Atlas — the activity pill and its details panel, drawn in the dock's Native Plus style (dock-style.hpp): the
// pill is a capsule in the notice fill, like the Replay Buffer pill above it, and the panel an elevated card, like the
// toast. Steps are the dock's stroke icons in the theme's success and warning colours rather than text symbols, and all
// text is painted, so the OBS style sheet can't override its muted colour. The interaction and motion follow Arc's
// public Agent Status documentation, implemented independently (docs/handover/activity-status.md).
#include "activity-status.hpp"

#include "dock-controls.hpp"
#include "dock-style.hpp"
#include "motion-preference.hpp"

#include <obs-module.h>

#include <QAbstractButton>
#include <QAccessibleWidget>
#include <QApplication>
#include <QElapsedTimer>
#include <QFontMetrics>
#include <QFrame>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QProgressBar>
#include <QResizeEvent>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariantAnimation>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>

namespace clip_atlas {
namespace {

// The pill: its height, padding and gaps in font heights, as the dock's chips; the leading mark's box; and its text, a
// little smaller than the theme's font, as the Replay Buffer pill's.
constexpr qreal kPillHeight = 1.5;
constexpr qreal kPillPadding = 0.55;
constexpr qreal kPillGap = 0.45;
constexpr qreal kMarkSide = 0.95;
constexpr qreal kLabelScale = 0.92;
constexpr qreal kTimerScale = 0.86;
constexpr qreal kChevronSide = 0.8;
// The pill shows its timer only when it has this much room, in font heights, so a narrow dock keeps the label.
constexpr qreal kTimerRoom = 11.0;
// The panel: a card as wide as the dock allows, up to kMaxCardWidth, with its header pill inset from its edge.
constexpr qreal kCardRadius = 10.0;
constexpr int kCardInset = 6;
constexpr int kMaxCardWidth = 384;
constexpr int kMaxCardHeight = 420;
// The panel's text, and its smaller lines: paths, the count and the history heading.
constexpr qreal kBodyScale = 0.92;
constexpr qreal kSmallScale = 0.86;

QString module_text(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

bool live(const Activity &a)
{
	return a.state == ActivityState::working || a.state == ActivityState::waiting;
}

QString kind_name(ClipKind kind)
{
	return module_text(kind == ClipKind::recording    ? "Activity.Recording"
			   : kind == ClipKind::screenshot ? "Activity.Screenshot"
							  : "Activity.Replay");
}

QString duration(ActivityClock::duration elapsed)
{
	const auto seconds =
		std::max<std::int64_t>(0, std::chrono::duration_cast<std::chrono::seconds>(elapsed).count());
	return QStringLiteral("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

QString notice(ActivityNotice value)
{
	switch (value) {
	case ActivityNotice::stability_timeout:
		return module_text("Activity.StabilityTimeout");
	case ActivityNotice::remux_unavailable:
		return module_text("Activity.RemuxUnavailable");
	case ActivityNotice::original_kept:
		return module_text("Activity.OriginalKept");
	case ActivityNotice::none:
		return {};
	}
	return {};
}

QString label(const Activity &a)
{
	if (a.state == ActivityState::failed)
		return module_text("Activity.Failed").arg(kind_name(a.kind));
	if (a.state == ActivityState::cancelled)
		return module_text("Activity.Cancelled");
	if (a.state == ActivityState::completed) {
		const bool warning = std::any_of(a.files.begin(), a.files.end(), [](const ActivityFile &f) {
			return f.notice != ActivityNotice::none;
		});
		return module_text(warning ? "Activity.CompletedNotice" : "Activity.Completed").arg(kind_name(a.kind));
	}
	const bool moving = std::any_of(a.files.begin(), a.files.end(),
					[](const ActivityFile &f) { return f.phase == ActivityPhase::moving; });
	const bool queued = std::any_of(a.files.begin(), a.files.end(),
					[](const ActivityFile &f) { return f.phase == ActivityPhase::queued; });
	if (moving)
		return module_text("Activity.Sorting")
			.arg(kind_name(a.kind),
			     a.game.empty() ? module_text("Activity.Unsorted") : QString::fromStdString(a.game));
	if (queued)
		return module_text("Activity.Queued").arg(kind_name(a.kind));
	const bool remux = std::any_of(a.files.begin(), a.files.end(), [](const ActivityFile &f) {
		return f.remuxed && f.phase == ActivityPhase::waiting;
	});
	return module_text(remux ? "Activity.WaitingRemux" : "Activity.WaitingFile");
}

// What a pill's or a history entry's leading mark shows. The label says the same in words, so colour is never the
// only signal.
enum class Mark {
	ready,
	working,
	waiting,
	completed,
	failed,
	cancelled,
};

Mark mark_of(const Activity &a)
{
	switch (a.state) {
	case ActivityState::working:
		return Mark::working;
	case ActivityState::waiting:
		return Mark::waiting;
	case ActivityState::completed:
		return Mark::completed;
	case ActivityState::failed:
		return Mark::failed;
	case ActivityState::cancelled:
		return Mark::cancelled;
	}
	return Mark::ready;
}

// A step of a file in the panel. Step text carries the meaning; the icon repeats it in the theme's colours.
enum class Step {
	done,
	current,
	pending,
	failed,
	stopped,
	notice,
};

// An outcome's badge: its icon in the theme's colour over a faint disc of the same colour, as the toast's.
void paint_badge(QPainter &p, DockIcon icon, const QRectF &box, const QColor &tone)
{
	p.save();
	p.setPen(Qt::NoPen);
	p.setBrush(with_alpha(tone, 0.2));
	p.drawEllipse(box);
	const qreal inset = box.width() * 0.17;
	paint_dock_icon(p, icon, box.adjusted(inset, inset, -inset, -inset), tone);
	p.restore();
}

// A faint ring with an arc: the accent arc turning while work is under way, or a still muted quarter for a wait.
void paint_ring(QPainter &p, const QRectF &box, const QPalette &pal, bool working, qreal spin)
{
	p.save();
	const qreal stroke = std::max<qreal>(1.5, box.width() * 0.13);
	const qreal half = stroke / 2.0 + 0.5;
	const QRectF ring = box.adjusted(half, half, -half, -half);
	p.setBrush(Qt::NoBrush);
	p.setPen(QPen(with_alpha(pal.color(QPalette::Text), 0.16), stroke));
	p.drawEllipse(ring);
	p.setPen(QPen(working ? accent_text(pal) : muted_text(pal), stroke, Qt::SolidLine, Qt::RoundCap));
	if (working)
		p.drawArc(ring, qRound(-spin * 16.0), 100 * 16);
	else
		p.drawArc(ring, 90 * 16, -90 * 16);
	p.restore();
}

void paint_dot(QPainter &p, const QRectF &box, qreal fraction, const QColor &color)
{
	p.save();
	const qreal side = std::round(box.width() * fraction);
	p.setPen(Qt::NoPen);
	p.setBrush(color);
	p.drawEllipse(QRectF(box.center().x() - side / 2.0, box.center().y() - side / 2.0, side, side));
	p.restore();
}

void paint_mark(QPainter &p, Mark mark, const QRectF &box, const QPalette &pal, qreal spin)
{
	const ThemeAccents &accents = theme_accents();
	switch (mark) {
	case Mark::ready:
		// Idle, as a stopped Replay Buffer is: a small muted dot.
		paint_dot(p, box, 0.46, muted_text(pal));
		break;
	case Mark::working:
		paint_ring(p, box, pal, true, spin);
		break;
	case Mark::waiting:
		paint_ring(p, box, pal, false, spin);
		break;
	case Mark::completed:
		paint_badge(p, DockIcon::check, box, accents.success);
		break;
	case Mark::failed:
		paint_badge(p, DockIcon::warning, box, accents.warning);
		break;
	case Mark::cancelled:
		paint_badge(p, DockIcon::clear, box, muted_text(pal));
		break;
	}
}

void paint_step(QPainter &p, Step step, const QRectF &box, const QPalette &pal)
{
	const ThemeAccents &accents = theme_accents();
	switch (step) {
	case Step::done:
		paint_dock_icon(p, DockIcon::check, box, accents.success);
		break;
	case Step::failed:
	case Step::notice:
		paint_dock_icon(p, DockIcon::warning, box, accents.warning);
		break;
	case Step::stopped:
		paint_dock_icon(p, DockIcon::clear, box.adjusted(1.0, 1.0, -1.0, -1.0), muted_text(pal));
		break;
	case Step::current: {
		// Under way now: the accent dot inside a faint ring.
		p.save();
		p.setBrush(Qt::NoBrush);
		p.setPen(QPen(with_alpha(accent_text(pal), 0.35), 1.5));
		p.drawEllipse(box.adjusted(2.0, 2.0, -2.0, -2.0));
		p.restore();
		paint_dot(p, box, 0.36, accent_text(pal));
		break;
	}
	case Step::pending:
		p.save();
		p.setBrush(Qt::NoBrush);
		p.setPen(QPen(muted_text(pal), 1.5));
		p.drawEllipse(box.adjusted(3.0, 3.0, -3.0, -3.0));
		p.restore();
		break;
	}
}

// The pill. Its copy at the top of the panel is flat: it sits on the card, so it has no fill of its own.
class ActivityPill : public QAbstractButton {
public:
	ActivityPill(QWidget *parent, bool flat) : QAbstractButton(parent), flat_(flat)
	{
		setFocusPolicy(Qt::StrongFocus);
		setCheckable(true);
		setCursor(Qt::PointingHandCursor);
		setAttribute(Qt::WA_Hover);
		setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
		roll_.setDuration(180);
		roll_.setEasingCurve(QEasingCurve::OutCubic);
		connect(&roll_, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
			transition_ = v.toReal();
			update();
		});
		width_motion_.setDuration(240);
		width_motion_.setEasingCurve(QEasingCurve::OutCubic);
		connect(&width_motion_, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
			display_width_ = v.toInt();
			updateGeometry();
		});
		clock_.start();
	}

	QSize sizeHint() const override
	{
		ensurePolished();
		return {display_width_ > 0 ? display_width_ : wanted_width(message_, !elapsed_.isEmpty()),
			pill_height()};
	}
	QSize minimumSizeHint() const override
	{
		ensurePolished();
		return {pill_height() * 3, pill_height()};
	}

	void present(const QString &message, Mark mark, const QString &elapsed, bool reduced)
	{
		reduced_ = reduced;
		elapsed_ = elapsed;
		if (message != message_ || mark != mark_) {
			previous_ = message_;
			message_ = message;
			mark_ = mark;
			roll_.stop();
			transition_ = 1;
			if (!reduced && isVisible() && !previous_.isEmpty()) {
				roll_.setStartValue(0.0);
				roll_.setEndValue(1.0);
				roll_.start();
			}
			setAccessibleName(message);
			setToolTip(message);
			QAccessibleEvent announcement(this, QAccessible::NameChanged);
			QAccessible::updateAccessibility(&announcement);
			const int target = wanted_width(message_, !elapsed.isEmpty());
			width_motion_.stop();
			if (!reduced && isVisible() && display_width_ > 0) {
				width_motion_.setStartValue(display_width_);
				width_motion_.setEndValue(target);
				width_motion_.start();
			} else {
				display_width_ = target;
			}
			updateGeometry();
		}
		if (reduced) {
			roll_.stop();
			transition_ = 1;
			if (width_motion_.state() == QAbstractAnimation::Running) {
				display_width_ = width_motion_.endValue().toInt();
				width_motion_.stop();
				updateGeometry();
			}
		}
		update();
	}

	void expansion(bool expanded)
	{
		setChecked(expanded);
		setAccessibleDescription(module_text(expanded ? "Activity.Expanded" : "Activity.Collapsed"));
		QAccessible::State changed;
		changed.expanded = true;
		changed.collapsed = true;
		QAccessibleStateChangeEvent event(this, changed);
		QAccessible::updateAccessibility(&event);
	}

	QString elapsed() const { return elapsed_; }

protected:
	void hideEvent(QHideEvent *event) override
	{
		roll_.stop();
		if (width_motion_.state() == QAbstractAnimation::Running)
			display_width_ = width_motion_.endValue().toInt();
		width_motion_.stop();
		transition_ = 1;
		QAbstractButton::hideEvent(event);
	}

	void changeEvent(QEvent *event) override
	{
		QAbstractButton::changeEvent(event);
		// The theme's font sets the pill's height and the room its label needs.
		if (event->type() == QEvent::FontChange) {
			display_width_ = wanted_width(message_, !elapsed_.isEmpty());
			updateGeometry();
		}
		if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange)
			update();
	}

	void keyPressEvent(QKeyEvent *event) override
	{
		if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
			click();
			event->accept();
			return;
		}
		QAbstractButton::keyPressEvent(event);
	}

	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing);
		const QPalette &pal = palette();
		const int font_height = QFontMetrics(font()).height();
		const QRectF shell = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
		const qreal radius = shell.height() / 2.0;

		// The notice fill, as the Replay Buffer pill's, then a shade towards the text under the mouse, while pressed
		// and while the panel is open. The panel's flat copy shades only.
		p.setPen(Qt::NoPen);
		if (!flat_) {
			p.setBrush(notice_fill(pal));
			p.drawRoundedRect(shell, radius, radius);
		}
		qreal shade = 0.0;
		if (isDown())
			shade = 0.12;
		else if (testAttribute(Qt::WA_UnderMouse))
			shade = 0.07;
		else if (isChecked() && !flat_)
			shade = 0.07;
		if (shade > 0.0) {
			p.setBrush(with_alpha(pal.color(QPalette::Text), shade));
			p.drawRoundedRect(shell, radius, radius);
		}

		const int padding = qRound(font_height * kPillPadding);
		const int gap = qRound(font_height * kPillGap);
		const qreal mark_side = std::round(font_height * kMarkSide);
		const QRectF mark_box(padding, (height() - mark_side) / 2.0, mark_side, mark_side);
		const qreal spin = reduced_ ? 45.0 : std::fmod(static_cast<double>(clock_.elapsed()) * 0.24, 360.0);
		p.save();
		// An outcome's badge fades in as its label rolls in.
		if (mark_ == Mark::completed || mark_ == Mark::failed)
			p.setOpacity(std::max(0.3, transition_));
		paint_mark(p, mark_, mark_box, pal, spin);
		p.restore();

		const qreal chevron_side = std::round(font_height * kChevronSide);
		const QRectF chevron_box(width() - padding - chevron_side, (height() - chevron_side) / 2.0,
					 chevron_side, chevron_side);
		paint_dock_icon(p, isChecked() ? DockIcon::chevron_up : DockIcon::chevron_down, chevron_box,
				muted_text(pal));

		const bool timer_visible = !elapsed_.isEmpty() && width() >= qRound(font_height * kTimerRoom);
		const int timer_room = timer_visible ? timer_width() + gap : 0;
		const int text_left = qRound(mark_box.right()) + gap;
		const int text_right = qRound(chevron_box.left()) - gap - timer_room;
		const QRect text_room(text_left, 0, std::max(0, text_right - text_left), height());
		const QFont label = label_font();
		const QFontMetrics label_metrics(label);
		// Idle reads as a hint, not news: Ready is muted.
		const QColor ink = mark_ == Mark::ready ? muted_text(pal) : pal.color(QPalette::Text);
		p.save();
		p.setClipRect(text_room);
		p.setFont(label);
		auto draw_label = [&](const QString &value, qreal opacity, int y) {
			p.setOpacity(opacity);
			p.setPen(ink);
			if (mark_ == Mark::working && !reduced_) {
				// A soft sweep of the muted colour across the label while work is under way.
				const qreal sweep = std::fmod(static_cast<double>(clock_.elapsed()) / 2400.0, 1.0);
				QLinearGradient shimmer(text_room.left() - text_room.width() +
								sweep * text_room.width() * 3,
							0, text_room.left() + sweep * text_room.width() * 3, 0);
				shimmer.setColorAt(0, ink);
				shimmer.setColorAt(0.5, muted_text(pal));
				shimmer.setColorAt(1, ink);
				p.setPen(QPen(QBrush(shimmer), 1));
			}
			p.drawText(text_room.translated(0, y), Qt::AlignVCenter | Qt::AlignLeft | Qt::TextSingleLine,
				   label_metrics.elidedText(value, Qt::ElideRight, text_room.width()));
		};
		if (transition_ < 1)
			draw_label(previous_, 1 - transition_, qRound(-0.5 * font_height * transition_));
		draw_label(message_, transition_, qRound(0.5 * font_height * (1 - transition_)));
		p.restore();

		if (timer_visible) {
			// Digits in fixed cells keep the counter steady even with a proportional theme font; the colon keeps
			// its own width.
			const QFont timer = scaled_font(font(), kTimerScale);
			p.setFont(timer);
			p.setPen(muted_text(pal));
			int x = qRound(chevron_box.left()) - gap - text_cells_width(elapsed_);
			for (const QChar c : elapsed_) {
				const int cell = cell_width(c);
				p.drawText(QRect(x, 0, cell, height()), Qt::AlignCenter, QString(c));
				x += cell;
			}
		}
		paint_focus_ring(p, this, shell, radius);
	}

private:
	QFont label_font() const { return scaled_font(font(), kLabelScale); }

	int pill_height() const { return qRound(QFontMetrics(font()).height() * kPillHeight); }

	int cell_width(QChar c) const
	{
		const QFontMetrics metrics(scaled_font(font(), kTimerScale));
		return metrics.horizontalAdvance(c.isDigit() ? QChar(QLatin1Char('0')) : c);
	}

	int text_cells_width(const QString &text) const
	{
		int width = 0;
		for (const QChar c : text)
			width += cell_width(c);
		return width;
	}

	// "000:00" in the timer's cells, so the pill doesn't change width as the seconds count.
	int timer_width() const { return text_cells_width(QStringLiteral("000:00")); }

	int wanted_width(const QString &message, bool with_timer) const
	{
		const int font_height = QFontMetrics(font()).height();
		const int gap = qRound(font_height * kPillGap);
		int width = 2 * qRound(font_height * kPillPadding) + qRound(font_height * kMarkSide) + 2 * gap +
			    qCeil(QFontMetricsF(label_font()).horizontalAdvance(message)) + 1 +
			    qRound(font_height * kChevronSide);
		if (with_timer)
			width += timer_width() + gap;
		return width;
	}

	bool flat_;
	QString message_, previous_, elapsed_;
	Mark mark_ = Mark::ready;
	int display_width_ = 0;
	bool reduced_ = false;
	qreal transition_ = 1;
	QElapsedTimer clock_;
	QVariantAnimation roll_;
	QVariantAnimation width_motion_;
};

class AccessiblePill : public QAccessibleWidget {
public:
	explicit AccessiblePill(ActivityPill *pill) : QAccessibleWidget(pill, QAccessible::Button) {}
	QString text(QAccessible::Text role) const override
	{
		if (role == QAccessible::Value)
			return static_cast<ActivityPill *>(widget())->elapsed();
		return QAccessibleWidget::text(role);
	}
	QAccessible::State state() const override
	{
		auto result = QAccessibleWidget::state();
		result.expandable = true;
		result.expanded = static_cast<ActivityPill *>(widget())->isChecked();
		result.collapsed = !result.expanded;
		return result;
	}
	QStringList actionNames() const override { return {pressAction(), setFocusAction()}; }
	void doAction(const QString &action) override
	{
		if (action == pressAction())
			static_cast<ActivityPill *>(widget())->click();
		else
			QAccessibleWidget::doAction(action);
	}
	QStringList keyBindingsForAction(const QString &action) const override
	{
		return action == pressAction() ? QStringList{QStringLiteral("Space"), QStringLiteral("Enter")}
					       : QStringList{};
	}
};

int accessibility_users = 0;
QAccessibleInterface *accessible_factory(const QString &, QObject *object)
{
	if (auto *pill = dynamic_cast<ActivityPill *>(object))
		return new AccessiblePill(pill);
	return nullptr;
}

// The details panel: a popup holding an elevated card, with the card's shadow in a transparent margin around it. Its
// contents margins are that margin, so its layout and contentsRect() start at the card's edge. The system's own popup
// shadow is turned off: it would be square around the rounded card.
class ActivityPanel : public QFrame {
public:
	explicit ActivityPanel(QWidget *owner) : QFrame(owner, Qt::Popup | Qt::NoDropShadowWindowHint)
	{
		setObjectName(QStringLiteral("activityPanel"));
		setAttribute(Qt::WA_TranslucentBackground);
		setAttribute(Qt::WA_WindowPropagation);
		setContentsMargins(kCardShadowSide, kCardShadowTop, kCardShadowSide, kCardShadowBottom);
	}

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		paint_elevated_card(p, palette(), QRectF(contentsRect()), kCardRadius);
	}
};

// Clips the scrolling body, children included, to the card's rounded inner edge during the panel's fade.
class InformationSurfaceEffect : public QGraphicsOpacityEffect {
public:
	using QGraphicsOpacityEffect::QGraphicsOpacityEffect;

protected:
	void draw(QPainter *painter) override
	{
		QPoint offset;
		const QPixmap source = sourcePixmap(Qt::LogicalCoordinates, &offset, QGraphicsEffect::NoPad);
		if (source.isNull())
			return;
		QPixmap mask(source.size());
		mask.setDevicePixelRatio(source.devicePixelRatio());
		mask.fill(Qt::transparent);
		{
			QPainter shape(&mask);
			shape.setRenderHint(QPainter::Antialiasing);
			shape.setPen(Qt::NoPen);
			shape.setBrush(Qt::white);
			// The inner and outer curves share their corner centres at the card's inset.
			const qreal radius = kCardRadius - (kCardInset - 1);
			shape.drawRoundedRect(QRectF(QPointF(), source.deviceIndependentSize()), radius, radius);
		}
		QPixmap clipped(source.size());
		clipped.setDevicePixelRatio(source.devicePixelRatio());
		clipped.fill(Qt::transparent);
		{
			QPainter composite(&clipped);
			composite.drawPixmap(0, 0, source);
			composite.setCompositionMode(QPainter::CompositionMode_DestinationIn);
			composite.drawPixmap(0, 0, mask);
		}
		painter->save();
		// Scroll viewports can occlude the parent's backing store, so the card's own fill goes under the cutout.
		if (const auto *surface = qobject_cast<QWidget *>(parent()))
			painter->fillRect(QRectF(offset, source.deviceIndependentSize()),
					  elevated_fill(surface->palette()));
		painter->setOpacity(painter->opacity() * opacity());
		painter->drawPixmap(offset, clipped);
		painter->restore();
	}
};

enum class ProgressTone {
	working,
	done,
	failed,
};

// Keeps QProgressBar's accessible range and value; only its paint is the dock's: a thin rounded track with the accent
// filling it as files are sorted, the theme's green once all are, its amber when the save failed. A theme that sets a
// taller minimum for progress bars gets the same thin bar, centred.
class StepProgress : public QProgressBar {
public:
	using QProgressBar::QProgressBar;
	QSize sizeHint() const override { return {100, 4}; }

	void set_tone(ProgressTone tone)
	{
		tone_ = tone;
		update();
	}

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing);
		p.setPen(Qt::NoPen);
		const QRectF track(0.0, (height() - 4) / 2.0, width(), 4.0);
		p.setBrush(with_alpha(palette().color(QPalette::Text), 0.12));
		p.drawRoundedRect(track, 2.0, 2.0);
		if (maximum() > 0 && value() > 0) {
			const ThemeAccents &accents = theme_accents();
			p.setBrush(tone_ == ProgressTone::done     ? accents.success
				   : tone_ == ProgressTone::failed ? accents.warning
								   : palette().color(QPalette::Highlight));
			const qreal filled = std::max(track.height(), track.width() * value() / qreal(maximum()));
			p.drawRoundedRect(QRectF(track.left(), track.top(), filled, track.height()), 2.0, 2.0);
		}
	}

private:
	ProgressTone tone_ = ProgressTone::working;
};

// A line of the panel: an optional step icon, then text that wraps, then an optional time at the right in the muted
// colour. Painted, so its muted tone holds under the theme's style sheet; its accessible name is its text.
class StepRow : public QWidget {
public:
	struct Options {
		std::optional<Step> step;
		bool indent = false; // no icon, but text in line with the rows that have one
		TextTone tone = TextTone::normal;
		qreal scale = kBodyScale;
		QFont::Weight weight = QFont::Normal;
		QString trailing;
	};

	StepRow(const QString &text, const Options &options, QWidget *parent)
		: QWidget(parent),
		  text_(text),
		  options_(options)
	{
		QSizePolicy policy(QSizePolicy::Ignored, QSizePolicy::Preferred);
		policy.setHeightForWidth(true);
		setSizePolicy(policy);
		setAccessibleName(options.trailing.isEmpty() ? text : text + QStringLiteral(", ") + options.trailing);
	}

	bool hasHeightForWidth() const override { return true; }

	int heightForWidth(int width) const override
	{
		ensurePolished();
		const QFontMetrics metrics(text_font());
		const int room = std::max(1, width - text_left() - trailing_room());
		const int text_height =
			metrics.boundingRect(QRect(0, 0, room, 100000), Qt::TextWordWrap | Qt::AlignLeft, text_)
				.height();
		return std::max({text_height, metrics.height(), icon_side()});
	}

	QSize sizeHint() const override
	{
		ensurePolished();
		return {200, heightForWidth(200)};
	}

	QSize minimumSizeHint() const override
	{
		ensurePolished();
		return {text_left() + trailing_room() + QFontMetrics(text_font()).averageCharWidth() * 6,
			QFontMetrics(text_font()).height()};
	}

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing);
		const QPalette &pal = palette();
		const QFont used = text_font();
		const QFontMetrics metrics(used);
		// The icon sits level with the first line.
		if (options_.step) {
			const qreal side = icon_side();
			paint_step(p, *options_.step, QRectF(0.0, (metrics.height() - side) / 2.0, side, side), pal);
		}
		QColor color = pal.color(QPalette::Text);
		if (options_.tone == TextTone::muted)
			color = muted_text(pal);
		else if (options_.tone == TextTone::accent)
			color = accent_text(pal);
		p.setFont(used);
		p.setPen(color);
		const int room = std::max(0, width() - text_left() - trailing_room());
		p.drawText(QRect(text_left(), 0, room, height()), Qt::TextWordWrap | Qt::AlignLeft | Qt::AlignTop,
			   text_);
		if (!options_.trailing.isEmpty()) {
			const QFont small = scaled_font(font(), kSmallScale);
			p.setFont(small);
			p.setPen(muted_text(pal));
			p.drawText(QRect(width() - trailing_width(), 0, trailing_width(), metrics.height()),
				   Qt::AlignRight | Qt::AlignVCenter | Qt::TextSingleLine, options_.trailing);
		}
	}

	void changeEvent(QEvent *event) override
	{
		QWidget::changeEvent(event);
		if (event->type() == QEvent::FontChange)
			updateGeometry();
		if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange)
			update();
	}

private:
	QFont text_font() const { return scaled_font(font(), options_.scale, options_.weight); }

	int icon_side() const
	{
		return options_.step || options_.indent ? qRound(QFontMetrics(font()).height() * 0.85) : 0;
	}

	int text_left() const
	{
		return options_.step || options_.indent ? icon_side() + qRound(QFontMetrics(font()).height() * 0.45)
							: 0;
	}

	int trailing_width() const
	{
		if (options_.trailing.isEmpty())
			return 0;
		return qCeil(QFontMetricsF(scaled_font(font(), kSmallScale)).horizontalAdvance(options_.trailing)) + 1;
	}

	int trailing_room() const
	{
		return options_.trailing.isEmpty() ? 0 : trailing_width() + qRound(QFontMetrics(font()).height() * 0.6);
	}

	QString text_;
	Options options_;
};

// A save in the history: its outcome's mark, then its summary, elided. A flat row like the clip list's: filled under
// the mouse, tinted with the accent when it is the save the panel shows.
class HistoryEntry : public QAbstractButton {
public:
	HistoryEntry(Mark mark, const QString &text, bool shown, QWidget *parent)
		: QAbstractButton(parent),
		  mark_(mark),
		  shown_(shown)
	{
		setText(text);
		setObjectName(QStringLiteral("activityHistoryEntry"));
		setFocusPolicy(Qt::StrongFocus);
		setCursor(Qt::PointingHandCursor);
		setAttribute(Qt::WA_Hover);
		setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
	}

	QSize sizeHint() const override
	{
		ensurePolished();
		return {120, qRound(QFontMetrics(font()).height() * 1.75)};
	}
	QSize minimumSizeHint() const override { return sizeHint(); }

protected:
	void keyPressEvent(QKeyEvent *event) override
	{
		// Enter chooses a save, as it opens the pill.
		if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
			click();
			event->accept();
			return;
		}
		QAbstractButton::keyPressEvent(event);
	}

	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing);
		const QPalette &pal = palette();
		const int font_height = QFontMetrics(font()).height();
		const QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
		p.setPen(Qt::NoPen);
		if (shown_) {
			p.setBrush(with_alpha(pal.color(QPalette::Highlight), 0.24));
			p.drawRoundedRect(box, 6.0, 6.0);
		}
		qreal shade = 0.0;
		if (isDown())
			shade = 0.12;
		else if (testAttribute(Qt::WA_UnderMouse))
			shade = 0.07;
		if (shade > 0.0) {
			p.setBrush(with_alpha(pal.color(QPalette::Text), shade));
			p.drawRoundedRect(box, 6.0, 6.0);
		}
		const int padding = qRound(font_height * 0.45);
		const qreal side = std::round(font_height * 0.85);
		paint_mark(p, mark_, QRectF(padding, (height() - side) / 2.0, side, side), pal, 45.0);
		const QFont label = scaled_font(font(), kBodyScale);
		const int text_left = padding + qRound(side) + qRound(font_height * 0.45);
		const QRect text_room(text_left, 0, std::max(0, width() - text_left - padding), height());
		p.setFont(label);
		p.setPen(pal.color(QPalette::Text));
		p.drawText(text_room, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine,
			   QFontMetrics(label).elidedText(text(), Qt::ElideRight, text_room.width()));
		paint_focus_ring(p, this, box, 6.0);
	}

	void changeEvent(QEvent *event) override
	{
		QAbstractButton::changeEvent(event);
		if (event->type() == QEvent::FontChange)
			updateGeometry();
		if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange)
			update();
	}

private:
	Mark mark_;
	bool shown_;
};

} // namespace

struct ActivityStatus::Impl {
	ActivityStatus *owner;
	ActivityModel model;
	ActivityPill *pill;
	ActivityPanel *panel;
	ActivityPill *panel_pill;
	QScrollArea *scroll;
	QTimer clock, frames, settle;
	QVariantAnimation expansion, fade;
	QGraphicsOpacityEffect *opacity;
	bool reduced = prefers_reduced_motion();
	bool motion_override = false;
	bool stopped = false;
	bool expanded = false;
	bool settled = false;
	std::uint64_t selected = 0;
	std::uint64_t settling_id = 0;

	explicit Impl(ActivityStatus *widget) : owner(widget)
	{
		if (accessibility_users++ == 0)
			QAccessible::installFactory(accessible_factory);
		pill = new ActivityPill(owner, false);
		pill->setObjectName(QStringLiteral("activityPill"));
		auto *root = new QHBoxLayout(owner);
		root->setContentsMargins(4, 0, 4, 0);
		root->addWidget(pill);
		root->addStretch();
		panel = new ActivityPanel(owner);
		panel->setAccessibleName(module_text("Activity.Details"));
		auto *layout = new QVBoxLayout(panel);
		layout->setContentsMargins(kCardInset, kCardInset, kCardInset, kCardInset);
		layout->setSpacing(kCardInset);
		panel_pill = new ActivityPill(panel, true);
		panel_pill->setObjectName(QStringLiteral("activityPanelPill"));
		layout->addWidget(panel_pill);
		layout->addWidget(new HairLine(Qt::Horizontal, panel));
		scroll = new QScrollArea(panel);
		scroll->setFrameShape(QFrame::NoFrame);
		scroll->setWidgetResizable(true);
		scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		scroll->setAutoFillBackground(false);
		scroll->viewport()->setAutoFillBackground(false);
		layout->addWidget(scroll, 1);
		opacity = new InformationSurfaceEffect(scroll);
		scroll->setGraphicsEffect(opacity);
		QObject::connect(pill, &QAbstractButton::clicked, owner, [this] { owner->set_expanded(!expanded); });
		QObject::connect(panel_pill, &QAbstractButton::clicked, owner, [this] { owner->set_expanded(false); });
		QObject::connect(&clock, &QTimer::timeout, owner, [this] { refresh(); });
		clock.setInterval(1000);
		frames.setInterval(33);
		QObject::connect(&frames, &QTimer::timeout, owner, [this] {
			if (pill->isVisible())
				pill->update();
			if (panel_pill->isVisible())
				panel_pill->update();
		});
		settle.setSingleShot(true);
		settle.setInterval(4000);
		QObject::connect(&settle, &QTimer::timeout, owner, [this] {
			settled = true;
			refresh();
		});
		expansion.setEasingCurve(QEasingCurve::OutCubic);
		QObject::connect(&expansion, &QVariantAnimation::valueChanged, owner,
				 [this](const QVariant &v) { panel->setGeometry(v.toRect()); });
		QObject::connect(&expansion, &QVariantAnimation::finished, owner, [this] {
			if (!expanded) {
				panel->hide();
				if (!stopped && owner->isVisible()) {
					owner->window()->activateWindow();
					pill->setFocus(Qt::PopupFocusReason);
				}
			}
		});
		QObject::connect(&fade, &QVariantAnimation::valueChanged, owner,
				 [this](const QVariant &v) { opacity->setOpacity(v.toReal()); });
		refresh();
	}

	~Impl()
	{
		panel->removeEventFilter(owner);
		if (--accessibility_users == 0)
			QAccessible::removeFactory(accessible_factory);
	}

	const Activity *shown() const
	{
		if (expanded && selected) {
			for (const auto &a : model.activities()) {
				if (a.save_id == selected)
					return &a;
			}
		}
		return model.current();
	}

	void refresh()
	{
		const auto *current = model.current();
		const auto *details = shown();
		auto present = [&](ActivityPill *control, const Activity *a, bool ready) {
			QString message = module_text("Activity.Ready"), elapsed;
			Mark mark = Mark::ready;
			if (a && !ready) {
				message = label(*a);
				mark = mark_of(*a);
				elapsed = duration((live(*a) ? ActivityClock::now() : a->ended_at) - a->started_at);
			}
			control->present(message, mark, elapsed, reduced || stopped);
			if (a && a->state == ActivityState::failed) {
				QString failure = message;
				for (const auto &file : a->files) {
					if (!file.detail.empty())
						failure += QStringLiteral(". ") + QString::fromStdString(file.detail);
				}
				if (control->accessibleName() != failure)
					control->setAccessibleName(failure);
			}
		};
		present(pill, current, settled && current && !live(*current));
		present(panel_pill, details, false);
		const bool ticking = !stopped && owner->isVisible() && current && live(*current);
		if (ticking)
			clock.start();
		else
			clock.stop();
		if (ticking && !reduced &&
		    (current->state == ActivityState::working ||
		     (expanded && details && details->state == ActivityState::working)))
			frames.start();
		else
			frames.stop();
		if (current && current->state == ActivityState::completed && current->save_id != settling_id) {
			settling_id = current->save_id;
			settle.start();
		}
	}

	// The panel's window when it is closed: its card just around the pill, its shadow's room outside that.
	QRect collapsed_geometry() const
	{
		const QRect card(pill->mapToGlobal(QPoint(-kCardInset, -kCardInset)),
				 pill->size() + QSize(2 * kCardInset, 2 * kCardInset));
		return card.adjusted(-kCardShadowSide, -kCardShadowTop, kCardShadowSide, kCardShadowBottom);
	}

	// Open: the card as wide as the dock allows up to kMaxCardWidth, as tall as its content up to kMaxCardHeight, its
	// header pill over the pill it opens from, kept on the screen.
	QRect target_geometry() const
	{
		const QRect screen = owner->screen()->availableGeometry().adjusted(6, 6, -6, -6);
		// The card starts just left of the pill, and leaves the same margin at the dock's right edge.
		const int side = std::max(0, pill->mapTo(owner, QPoint(0, 0)).x() - kCardInset);
		const int card_width = std::max(
			1, std::min({kMaxCardWidth, owner->width() - 2 * side, screen.width() - 2 * kCardShadowSide}));
		const QWidget *body = scroll->widget();
		const int body_width = std::max(1, card_width - 2 * kCardInset);
		const int content_height = body && body->layout()
						   ? (body->layout()->hasHeightForWidth()
							      ? body->layout()->totalHeightForWidth(body_width)
							      : body->layout()->sizeHint().height())
						   : 60;
		const int header = panel_pill->sizeHint().height() + 1 + 3 * kCardInset;
		const int card_height = std::min({kMaxCardHeight, screen.height() - kCardShadowTop - kCardShadowBottom,
						  std::max(header + 60, header + content_height + kCardInset)});
		QPoint card_at = pill->mapToGlobal(QPoint(-kCardInset, -kCardInset));
		card_at.setX(std::clamp(card_at.x(), screen.left() + kCardShadowSide,
					std::max(screen.left() + kCardShadowSide,
						 screen.right() + 1 - kCardShadowSide - card_width)));
		card_at.setY(std::clamp(card_at.y(), screen.top() + kCardShadowTop,
					std::max(screen.top() + kCardShadowTop,
						 screen.bottom() + 1 - kCardShadowBottom - card_height)));
		return QRect(card_at - QPoint(kCardShadowSide, kCardShadowTop),
			     QSize(card_width + 2 * kCardShadowSide, card_height + kCardShadowTop + kCardShadowBottom));
	}

	void rebuild()
	{
		const int position = scroll->verticalScrollBar()->value();
		const bool had_focus = scroll->isAncestorOf(QApplication::focusWidget());
		auto *body = new QWidget;
		body->setPalette(panel->palette());
		auto *rows = new QVBoxLayout(body);
		rows->setContentsMargins(kCardInset, 2, kCardInset, kCardInset);
		rows->setSpacing(6);
		const Activity *a = shown();
		if (!a) {
			StepRow::Options empty;
			empty.tone = TextTone::muted;
			rows->addWidget(new StepRow(module_text("Activity.Empty"), empty, body));
		} else {
			const auto completed =
				std::count_if(a->files.begin(), a->files.end(), [](const ActivityFile &f) {
					return f.phase == ActivityPhase::completed;
				});
			const QString count = module_text("Activity.Count").arg(completed).arg(a->files.size());
			// The save: received, with its count at the right, then how far it has got.
			StepRow::Options received;
			received.step = Step::done;
			received.weight = QFont::DemiBold;
			received.trailing = count;
			rows->addWidget(
				new StepRow(module_text("Activity.Accepted").arg(kind_name(a->kind)), received, body));
			auto *progress = new StepProgress(body);
			progress->setRange(0, static_cast<int>(a->files.size()));
			progress->setValue(static_cast<int>(completed));
			progress->setFixedHeight(6);
			progress->setAccessibleName(count);
			progress->set_tone(a->state == ActivityState::failed      ? ProgressTone::failed
					   : a->state == ActivityState::completed ? ProgressTone::done
										  : ProgressTone::working);
			rows->addWidget(progress);
			for (const auto &file : a->files) {
				rows->addSpacing(4);
				// The file, muted, then its steps under it. Paths are plain text and wrap even without spaces.
				QString path = QString::fromStdString(file.path);
				path.replace(QLatin1Char('/'), QStringLiteral("/\u200b"));
				path.replace(QLatin1Char('\\'), QStringLiteral("\\\u200b"));
				StepRow::Options name;
				name.tone = TextTone::muted;
				name.scale = kSmallScale;
				auto *path_row = new StepRow(path, name, body);
				path_row->setAccessibleName(QString::fromStdString(file.path));
				rows->addWidget(path_row);

				const bool waiting = file.phase == ActivityPhase::waiting;
				const bool complete = file.phase == ActivityPhase::completed;
				const bool failed = file.phase == ActivityPhase::failed;
				const bool cancelled = file.phase == ActivityPhase::cancelled;
				const bool missing = file.phase == ActivityPhase::unavailable;
				// Settling: done once confirmed, under way while waiting, a caveat when the wait ended unconfirmed.
				StepRow::Options ready;
				ready.step = file.stability_confirmed ? Step::done
					     : waiting                ? Step::current
								      : Step::notice;
				const char *ready_key =
					waiting ? (file.remuxed ? "Activity.Step.Remux" : "Activity.Step.Waiting")
					: file.stability_confirmed ? "Activity.Step.Ready"
								   : "Activity.Step.WaitEnded";
				rows->addWidget(new StepRow(module_text(ready_key), ready, body));

				// Sorting.
				const char *move_key = waiting     ? "Activity.Step.Pending"
						       : complete  ? "Activity.Step.Completed"
						       : failed    ? "Activity.Step.Failed"
						       : cancelled ? "Activity.Step.Cancelled"
						       : missing   ? "Activity.Step.Unavailable"
						       : file.phase == ActivityPhase::moving ? "Activity.Step.Moving"
											     : "Activity.Step.Queued";
				StepRow::Options move;
				move.step = complete                              ? Step::done
					    : failed                              ? Step::failed
					    : (cancelled || missing)              ? Step::stopped
					    : file.phase == ActivityPhase::moving ? Step::current
										  : Step::pending;
				if (waiting)
					move.tone = TextTone::muted;
				if (file.phase >= ActivityPhase::completed)
					move.trailing = duration(file.changed_at - a->started_at);
				rows->addWidget(new StepRow(module_text(move_key), move, body));

				if (file.notice != ActivityNotice::none) {
					StepRow::Options caveat;
					caveat.step = Step::notice;
					rows->addWidget(new StepRow(notice(file.notice), caveat, body));
				}
				if (!file.detail.empty()) {
					// The backend's reason, in line with the step it explains.
					StepRow::Options detail;
					detail.indent = true;
					rows->addWidget(new StepRow(
						module_text("Activity.Detail").arg(QString::fromStdString(file.detail)),
						detail, body));
				}
			}
		}
		if (model.activities().size() > 1) {
			rows->addSpacing(6);
			rows->addWidget(new HairLine(Qt::Horizontal, body));
			rows->addSpacing(2);
			// A heading like the clip list's day headers.
			StepRow::Options heading;
			heading.scale = kSmallScale;
			heading.weight = QFont::DemiBold;
			rows->addWidget(new StepRow(module_text("Activity.Recent"), heading, body));
			const std::uint64_t shown_id = a ? a->save_id : 0;
			for (auto it = model.activities().rbegin(); it != model.activities().rend(); ++it) {
				const QString summary =
					module_text("Activity.HistoryEntry")
						.arg(label(*it), it->game.empty() ? module_text("Activity.Unsorted")
										  : QString::fromStdString(it->game))
						.arg(it->save_id);
				auto *entry = new HistoryEntry(mark_of(*it), summary, it->save_id == shown_id, body);
				entry->setToolTip(summary);
				entry->setAccessibleName(module_text("Activity.Inspect").arg(summary));
				QObject::connect(entry, &QAbstractButton::clicked, owner, [this, id = it->save_id] {
					selected = id;
					rebuild();
					refresh();
					panel_pill->setFocus(Qt::OtherFocusReason);
				});
				rows->addWidget(entry);
			}
		}
		rows->addStretch();
		// A history entry can rebuild this panel from its own click handler.
		if (auto *previous = scroll->takeWidget()) {
			previous->hide();
			previous->deleteLater();
		}
		scroll->setWidget(body);
		// QScrollArea fills the widget it holds with the window colour; the card's own fill shows through instead.
		body->setAutoFillBackground(false);
		scroll->verticalScrollBar()->setValue(position);
		if (had_focus)
			panel_pill->setFocus(Qt::OtherFocusReason);
		fade.stop();
		opacity->setOpacity(1);
		if (!reduced && expanded) {
			fade.setDuration(140);
			fade.setStartValue(0.4);
			fade.setEndValue(1.0);
			fade.start();
		}
		if (expanded && panel->isVisible()) {
			if (expansion.state() == QAbstractAnimation::Running)
				expansion.setEndValue(target_geometry());
			else
				panel->setGeometry(target_geometry());
		}
	}
};

ActivityStatus::ActivityStatus(QWidget *parent) : QWidget(parent), impl_(std::make_unique<Impl>(this))
{
	impl_->panel->installEventFilter(this);
	setObjectName(QStringLiteral("activityStatus"));
	setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
}

ActivityStatus::~ActivityStatus()
{
	stop();
}

void ActivityStatus::on_activity(const ActivityEvent &event)
{
	if (!impl_->model.apply(event))
		return;
	impl_->settled = false;
	if (impl_->expanded)
		impl_->rebuild();
	impl_->refresh();
}

void ActivityStatus::stop()
{
	if (impl_->stopped)
		return;
	impl_->stopped = true;
	impl_->model.stop();
	impl_->clock.stop();
	impl_->frames.stop();
	impl_->settle.stop();
	impl_->fade.stop();
	impl_->expansion.stop();
	impl_->panel->hide();
	impl_->refresh();
}

void ActivityStatus::set_expanded(bool expanded)
{
	auto &s = *impl_;
	if (s.stopped || expanded == s.expanded)
		return;
	s.expanded = expanded;
	s.pill->expansion(expanded);
	s.panel_pill->expansion(expanded);
	s.expansion.stop();
	if (expanded) {
		const QRect from = s.panel->isVisible() ? s.panel->geometry() : s.collapsed_geometry();
		s.selected = 0;
		s.panel->setPalette(palette());
		s.panel->setFont(font());
		s.rebuild();
		const QRect target = s.target_geometry();
		s.panel->setGeometry(from);
		s.panel->show();
		s.panel_pill->setFocus(Qt::PopupFocusReason);
		if (s.reduced) {
			s.panel->setGeometry(target);
		} else {
			s.expansion.setDuration(260);
			s.expansion.setStartValue(s.panel->geometry());
			s.expansion.setEndValue(target);
			s.expansion.start();
		}
	} else {
		if (s.reduced) {
			s.panel->hide();
			window()->activateWindow();
			s.pill->setFocus(Qt::PopupFocusReason);
		} else {
			s.expansion.setDuration(180);
			s.expansion.setStartValue(s.panel->geometry());
			s.expansion.setEndValue(s.collapsed_geometry());
			s.expansion.start();
		}
	}
	s.refresh();
}

bool ActivityStatus::is_expanded() const
{
	return impl_->expanded;
}

void ActivityStatus::set_reduced_motion(bool reduced)
{
	impl_->reduced = reduced;
	impl_->motion_override = true;
	impl_->expansion.stop();
	impl_->fade.stop();
	impl_->opacity->setOpacity(1);
	if (impl_->expanded)
		impl_->panel->setGeometry(impl_->target_geometry());
	else
		impl_->panel->hide();
	impl_->refresh();
}

bool ActivityStatus::eventFilter(QObject *object, QEvent *event)
{
	if (object == impl_->panel) {
		if (event->type() == QEvent::KeyPress && static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape) {
			set_expanded(false);
			return true;
		}
		if (event->type() == QEvent::Hide) {
			impl_->expansion.stop();
			impl_->fade.stop();
			impl_->expanded = false;
			impl_->pill->expansion(false);
			impl_->panel_pill->expansion(false);
		}
	}
	return QWidget::eventFilter(object, event);
}

void ActivityStatus::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);
	if (!impl_->motion_override)
		impl_->reduced = prefers_reduced_motion();
	impl_->refresh();
}

void ActivityStatus::hideEvent(QHideEvent *event)
{
	impl_->clock.stop();
	impl_->frames.stop();
	impl_->panel->hide();
	QWidget::hideEvent(event);
}

void ActivityStatus::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	if (impl_->expanded) {
		impl_->expansion.stop();
		impl_->panel->setGeometry(impl_->target_geometry());
	}
}

} // namespace clip_atlas
