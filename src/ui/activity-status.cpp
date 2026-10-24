// Clip Atlas — independently implemented from Arc's public interaction and motion documentation.
#include "activity-status.hpp"

#include "motion-preference.hpp"

#include <obs-module.h>

#include <QAbstractButton>
#include <QAccessibleWidget>
#include <QApplication>
#include <QElapsedTimer>
#include <QFrame>
#include <QGraphicsOpacityEffect>
#include <QHideEvent>
#include <QKeyEvent>
#include <QLabel>
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
#include <QToolButton>
#include <QVariantAnimation>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <functional>

namespace clip_atlas {
namespace {

constexpr qreal kPanelRadius = 17;
constexpr int kPanelInset = 6;

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

// The only continuously painted surface. It uses the host font and palette, including its focus colour.
class ActivityPill : public QAbstractButton {
public:
	explicit ActivityPill(QWidget *parent) : QAbstractButton(parent)
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

	QSize sizeHint() const override { return {display_width_, 36}; }
	QSize minimumSizeHint() const override { return {80, 36}; }

	void present(const QString &message, int state, const QString &elapsed, bool reduced)
	{
		reduced_ = reduced;
		elapsed_ = elapsed;
		if (message != message_ || state != state_) {
			previous_ = message_;
			message_ = message;
			state_ = state;
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
			updateGeometry();
			const int target = fontMetrics().horizontalAdvance(message_) + 64 +
					   (elapsed.isEmpty()
						    ? 0
						    : fontMetrics().horizontalAdvance(QStringLiteral("000:00")) + 12);
			width_motion_.stop();
			if (!reduced && isVisible()) {
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
		p.setFont(font());
		p.setRenderHint(QPainter::Antialiasing);
		const auto pal = palette();
		const QColor ink = pal.color(QPalette::Text);
		const QColor muted = pal.color(QPalette::PlaceholderText);
		const QColor accent = pal.color(QPalette::Highlight);
		const QRectF shell = QRectF(rect()).adjusted(1, 1, -1, -1);
		p.setBrush(pal.color(underMouse() || isDown() ? QPalette::Midlight : QPalette::Base));
		QColor border = pal.color(QPalette::Mid);
		border.setAlphaF(0.65);
		p.setPen(QPen(border, 1));
		p.drawRoundedRect(shell, 17, 17);
		if (hasFocus()) {
			p.setBrush(Qt::NoBrush);
			p.setPen(QPen(accent, 1.5));
			p.drawRoundedRect(shell.adjusted(2, 2, -2, -2), 14, 14);
		}
		const QPointF centre(20, height() / 2.0);
		const QRectF ring(centre.x() - 10, centre.y() - 10, 20, 20);
		p.setBrush(Qt::NoBrush);
		p.setPen(QPen(border, 1.3));
		p.drawEllipse(ring);
		const qreal spin = reduced_ ? 45 : std::fmod(clock_.elapsed() * 0.24, 360.0);
		p.setPen(QPen(state_ == 1 ? accent : ink, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
		if (state_ == 1)
			p.drawArc(ring, static_cast<int>(-spin * 16), 100 * 16);
		else if (state_ == 3 || state_ == 4)
			p.drawArc(ring, 90 * 16, static_cast<int>(-360 * 16 * transition_));
		QPainterPath glyph;
		if (state_ == 3) {
			glyph.moveTo(centre + QPointF(-4, 0));
			glyph.lineTo(centre + QPointF(-1, 3));
			glyph.lineTo(centre + QPointF(4, -3));
		} else if (state_ == 4 || state_ == 5) {
			glyph.moveTo(centre + QPointF(-3, -3));
			glyph.lineTo(centre + QPointF(3, 3));
			glyph.moveTo(centre + QPointF(3, -3));
			glyph.lineTo(centre + QPointF(-3, 3));
		} else {
			// An independent atlas mark, rather than the reference's agent logo.
			glyph.moveTo(centre + QPointF(0, -4));
			glyph.lineTo(centre + QPointF(4, 0));
			glyph.lineTo(centre + QPointF(0, 4));
			glyph.lineTo(centre + QPointF(-4, 0));
			glyph.closeSubpath();
		}
		p.drawPath(glyph);
		const bool timer_visible = width() >= 215 && !elapsed_.isEmpty();
		const int timer_width = timer_visible ? fontMetrics().horizontalAdvance(QStringLiteral("000:00")) + 12
						      : 0;
		const QRect text_room(39, 0, std::max(0, width() - 64 - timer_width), height());
		p.save();
		p.setClipRect(text_room);
		auto draw_label = [&](const QString &value, qreal opacity, int y) {
			p.setOpacity(opacity);
			p.setPen(ink);
			if (state_ == 1 && !reduced_) {
				const qreal sweep = std::fmod(clock_.elapsed() / 2400.0, 1.0);
				QLinearGradient shimmer(text_room.left() - text_room.width() +
								sweep * text_room.width() * 3,
							0, text_room.left() + sweep * text_room.width() * 3, 0);
				shimmer.setColorAt(0, ink);
				shimmer.setColorAt(0.5, pal.color(QPalette::PlaceholderText));
				shimmer.setColorAt(1, ink);
				p.setPen(QPen(QBrush(shimmer), 1));
			}
			p.drawText(text_room.translated(0, y), Qt::AlignVCenter | Qt::AlignLeft,
				   fontMetrics().elidedText(value, Qt::ElideRight, text_room.width()));
		};
		if (transition_ < 1)
			draw_label(previous_, 1 - transition_, static_cast<int>(-10 * transition_));
		draw_label(message_, transition_, static_cast<int>(10 * (1 - transition_)));
		p.restore();
		p.setPen(muted);
		if (timer_visible) {
			// Fixed cells keep the counter steady even with a proportional host font.
			const int cell = fontMetrics().horizontalAdvance(QLatin1Char('0'));
			int x = width() - 28 - elapsed_.size() * cell;
			for (const QChar c : elapsed_) {
				p.drawText(QRect(x, 0, cell, height()), Qt::AlignCenter, QString(c));
				x += cell;
			}
		}
		const qreal y = height() / 2.0;
		p.setPen(QPen(muted, 1.3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
		const qreal direction = isChecked() ? -1 : 1;
		p.drawLine(QPointF(width() - 18, y - direction * 2), QPointF(width() - 15, y + direction));
		p.drawLine(QPointF(width() - 15, y + direction), QPointF(width() - 12, y - direction * 2));
	}

private:
	QString message_, previous_, elapsed_;
	int state_ = 0;
	int display_width_ = 180;
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

class ActivityPanel : public QFrame {
public:
	explicit ActivityPanel(QWidget *owner) : QFrame(owner, Qt::Popup)
	{
		setObjectName(QStringLiteral("activityPanel"));
		setAttribute(Qt::WA_TranslucentBackground);
		setAttribute(Qt::WA_WindowPropagation);
	}

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing);
		p.setPen(QPen(palette().color(QPalette::Mid), 1));
		p.setBrush(palette().color(QPalette::Base));
		p.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), kPanelRadius, kPanelRadius);
	}
};

// Clip the complete information surface, including its scrolling children, during the existing fade.
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
			// The inner and outer curves share their corner centres at this uniform inset.
			const qreal radius = kPanelRadius - (kPanelInset - 1);
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
		// Scroll viewports can occlude the parent's backing store. Supply the menu surface beneath the cutout.
		if (const auto *surface = qobject_cast<QWidget *>(parent()))
			painter->fillRect(QRectF(offset, source.deviceIndependentSize()),
					  surface->palette().color(QPalette::Base));
		painter->setOpacity(painter->opacity() * opacity());
		painter->drawPixmap(offset, clipped);
		painter->restore();
	}
};

// Keep QProgressBar's accessibility value interface; only its paint is specialised.
class StepProgress : public QProgressBar {
public:
	using QProgressBar::QProgressBar;
	QSize sizeHint() const override { return {100, 4}; }

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing);
		p.setPen(Qt::NoPen);
		p.setBrush(palette().color(QPalette::Mid));
		p.drawRoundedRect(QRectF(rect()), 2, 2);
		if (maximum() > 0) {
			p.setBrush(palette().color(QPalette::Highlight));
			p.drawRoundedRect(QRectF(0, 0, width() * value() / qreal(maximum()), height()), 2, 2);
		}
	}
};

QLabel *wrapped_label(const QString &value, QWidget *parent, bool secondary = false)
{
	auto *result = new QLabel(value, parent);
	result->setTextFormat(Qt::PlainText);
	result->setWordWrap(true);
	auto policy = result->sizePolicy();
	policy.setHorizontalPolicy(QSizePolicy::Ignored);
	result->setSizePolicy(policy);
	if (secondary) {
		auto pal = result->palette();
		pal.setColor(QPalette::WindowText, pal.color(QPalette::PlaceholderText));
		result->setPalette(pal);
	}
	return result;
}

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
		pill = new ActivityPill(owner);
		pill->setObjectName(QStringLiteral("activityPill"));
		auto *root = new QHBoxLayout(owner);
		root->setContentsMargins(4, 0, 4, 0);
		root->addWidget(pill);
		root->addStretch();
		panel = new ActivityPanel(owner);
		panel->setAccessibleName(module_text("Activity.Details"));
		auto *layout = new QVBoxLayout(panel);
		layout->setContentsMargins(kPanelInset, kPanelInset, kPanelInset, kPanelInset);
		layout->setSpacing(8);
		panel_pill = new ActivityPill(panel);
		panel_pill->setObjectName(QStringLiteral("activityPanelPill"));
		layout->addWidget(panel_pill);
		scroll = new QScrollArea(panel);
		scroll->setFrameShape(QFrame::NoFrame);
		scroll->setWidgetResizable(true);
		scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
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
			int state = 0;
			if (a && !ready) {
				message = label(*a);
				state = a->state == ActivityState::working     ? 1
					: a->state == ActivityState::waiting   ? 2
					: a->state == ActivityState::completed ? 3
					: a->state == ActivityState::failed    ? 4
									       : 5;
				elapsed = duration((live(*a) ? ActivityClock::now() : a->ended_at) - a->started_at);
			}
			control->present(message, state, elapsed, reduced || stopped);
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

	QRect target_geometry() const
	{
		const QRect screen = owner->screen()->availableGeometry().adjusted(6, 6, -6, -6);
		const int width = std::max(1, std::min({384, owner->width(), screen.width()}));
		const QWidget *body = scroll->widget();
		const int content_height =
			body && body->layout() ? (body->layout()->hasHeightForWidth()
							  ? body->layout()->totalHeightForWidth(std::max(1, width - 36))
							  : body->layout()->sizeHint().height())
					       : 60;
		const int height = std::min({400, screen.height(), std::max(110, content_height + 64)});
		QPoint at = pill->mapToGlobal(QPoint(0, 0));
		at.setX(std::clamp(at.x(), screen.left(), screen.right() - width + 1));
		at.setY(std::clamp(at.y(), screen.top(), screen.bottom() - height + 1));
		return {at, QSize(width, height)};
	}

	void rebuild()
	{
		const int position = scroll->verticalScrollBar()->value();
		const bool had_focus = scroll->isAncestorOf(QApplication::focusWidget());
		auto *body = new QWidget;
		body->setPalette(panel->palette());
		auto *rows = new QVBoxLayout(body);
		rows->setContentsMargins(10, 0, 10, 0);
		rows->setSpacing(10);
		const Activity *a = shown();
		if (!a) {
			rows->addWidget(wrapped_label(module_text("Activity.Empty"), body, true));
		} else {
			rows->addWidget(wrapped_label(module_text("Activity.Accepted").arg(kind_name(a->kind)), body));
			const auto completed =
				std::count_if(a->files.begin(), a->files.end(), [](const ActivityFile &f) {
					return f.phase == ActivityPhase::completed;
				});
			const QString count = module_text("Activity.Count").arg(completed).arg(a->files.size());
			auto *progress = new StepProgress(body);
			progress->setRange(0, static_cast<int>(a->files.size()));
			progress->setValue(static_cast<int>(completed));
			progress->setFixedHeight(4);
			progress->setAccessibleName(count);
			rows->addWidget(progress);
			rows->addWidget(wrapped_label(count, body, true));
			for (const auto &file : a->files) {
				auto *group = new QWidget(body);
				auto *steps = new QVBoxLayout(group);
				steps->setContentsMargins(0, 4, 0, 4);
				steps->setSpacing(6);
				// Paths are plain text and wrap even when no spaces are available.
				QString path = QString::fromStdString(file.path);
				path.replace(QLatin1Char('/'), QStringLiteral("/\u200b"));
				path.replace(QLatin1Char('\\'), QStringLiteral("\\\u200b"));
				auto *name = wrapped_label(path, group, true);
				name->setAccessibleName(QString::fromStdString(file.path));
				steps->addWidget(name);
				const bool waiting = file.phase == ActivityPhase::waiting;
				const bool complete = file.phase == ActivityPhase::completed;
				const bool failed = file.phase == ActivityPhase::failed;
				const bool cancelled = file.phase == ActivityPhase::cancelled;
				const bool missing = file.phase == ActivityPhase::unavailable;
				QString ready = module_text(
					waiting ? (file.remuxed ? "Activity.Step.Remux" : "Activity.Step.Waiting")
					: file.stability_confirmed ? "Activity.Step.Ready"
								   : "Activity.Step.WaitEnded");
				steps->addWidget(wrapped_label(QString(file.stability_confirmed ? QChar(0x2713)
												: QChar(0x25cb)) +
								       QLatin1Char(' ') + ready,
							       group));
				const char *move_key = waiting     ? "Activity.Step.Pending"
						       : complete  ? "Activity.Step.Completed"
						       : failed    ? "Activity.Step.Failed"
						       : cancelled ? "Activity.Step.Cancelled"
						       : missing   ? "Activity.Step.Unavailable"
						       : file.phase == ActivityPhase::moving ? "Activity.Step.Moving"
											     : "Activity.Step.Queued";
				const QChar mark = complete                              ? QChar(0x2713)
						   : (failed || cancelled || missing)    ? QChar(0x00d7)
						   : file.phase == ActivityPhase::moving ? QChar(0x25c9)
											 : QChar(0x25cb);
				QString outcome = QString(mark) + QLatin1Char(' ') + module_text(move_key);
				if (file.phase >= ActivityPhase::completed)
					outcome += QStringLiteral("  ·  ") + duration(file.changed_at - a->started_at);
				steps->addWidget(wrapped_label(outcome, group, waiting));
				if (file.notice != ActivityNotice::none)
					steps->addWidget(wrapped_label(notice(file.notice), group));
				if (!file.detail.empty())
					steps->addWidget(wrapped_label(
						module_text("Activity.Detail").arg(QString::fromStdString(file.detail)),
						group));
				rows->addWidget(group);
			}
		}
		if (model.activities().size() > 1) {
			rows->addWidget(wrapped_label(module_text("Activity.Recent"), body, true));
			for (auto it = model.activities().rbegin(); it != model.activities().rend(); ++it) {
				auto *entry = new QToolButton(body);
				auto entry_palette = entry->palette();
				entry_palette.setColor(QPalette::ButtonText, entry_palette.color(QPalette::Text));
				entry->setPalette(entry_palette);
				entry->setAutoRaise(true);
				entry->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
				const QString summary =
					module_text("Activity.HistoryEntry")
						.arg(label(*it), it->game.empty() ? module_text("Activity.Unsorted")
										  : QString::fromStdString(it->game))
						.arg(it->save_id);
				entry->setText(summary);
				entry->setToolTip(summary);
				entry->setAccessibleName(module_text("Activity.Inspect").arg(summary));
				QObject::connect(entry, &QToolButton::clicked, owner, [this, id = it->save_id] {
					selected = id;
					rebuild();
					refresh();
					panel_pill->setFocus(Qt::OtherFocusReason);
				});
				rows->addWidget(entry);
			}
		}
		rows->addStretch();
		// A history button can rebuild this panel from its own click handler.
		if (auto *previous = scroll->takeWidget()) {
			previous->hide();
			previous->deleteLater();
		}
		scroll->setWidget(body);
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
		const QRect from = s.panel->isVisible() ? s.panel->geometry()
							: QRect(s.pill->mapToGlobal(QPoint(0, 0)), s.pill->size());
		s.selected = 0;
		s.panel->setPalette(palette());
		s.panel->setFont(font());
		s.rebuild();
		const QRect target = s.target_geometry();
		s.panel->setGeometry(from);
		s.panel->show();
		s.panel_pill->setFocus(Qt::PopupFocusReason);
		if (s.reduced)
			s.panel->setGeometry(target);
		else {
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
			s.expansion.setEndValue(QRect(s.pill->mapToGlobal(QPoint(0, 0)), s.pill->size()));
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
