// Clip Atlas — the note after a clip is filed. dock-toast.hpp shows it and says how long it stays.
#include "dock-toast.hpp"

#include "dock-controls.hpp"
#include "dock-style.hpp"

#include <obs-module.h>

#include <QEnterEvent>
#include <QEvent>
#include <QFontMetrics>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QPainter>
#include <QRectF>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariantAnimation>

#include <algorithm>
#include <cmath>
#include <utility>

namespace clip_atlas {

namespace {

// Room around the card for its shadow, as paint_elevated_card() paints it.
constexpr int kShadowSide = kCardShadowSide;
constexpr int kShadowTop = kCardShadowTop;
constexpr int kShadowBottom = kCardShadowBottom;
constexpr qreal kRadius = 10.0;
constexpr int kFadeInMs = 160;
constexpr int kFadeOutMs = 220;
// The card's padding, and the badge's size in font heights.
constexpr int kPaddingX = 12;
constexpr int kPaddingY = 10;
constexpr qreal kBadge = 1.35;

QString module_text(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

} // namespace

DockToast::DockToast(QWidget *parent) : QWidget(parent)
{
	title_ = new TextBlock(QString(), TextTone::normal, 1.0, QFont::DemiBold, this);
	title_->set_wrap(true);
	detail_ = new TextBlock(QString(), TextTone::muted, 0.92, QFont::Normal, this);
	detail_->set_wrap(true);
	show_ = new GhostButton(module_text("Dock.Toast.Show"), this);
	connect(show_, &QAbstractButton::clicked, this, [this] { run_action(); });

	auto *text = new QVBoxLayout;
	text->setSpacing(2);
	text->addWidget(title_);
	text->addWidget(detail_);
	auto *row = new QHBoxLayout(this);
	row->setSpacing(10);
	row->addLayout(text, 1);
	row->addWidget(show_, 0, Qt::AlignVCenter);

	countdown_ = new QTimer(this);
	countdown_->setSingleShot(true);
	connect(countdown_, &QTimer::timeout, this, [this] { dismiss(); });

	opacity_ = new QGraphicsOpacityEffect(this);
	opacity_->setOpacity(0.0);
	setGraphicsEffect(opacity_);
	fade_ = new QVariantAnimation(this);
	connect(fade_, &QVariantAnimation::valueChanged, this,
		[this](const QVariant &value) { opacity_->setOpacity(value.toReal()); });
	connect(fade_, &QVariantAnimation::finished, this, [this] {
		if (fade_->endValue().toReal() <= 0.0)
			hide();
	});
	setAttribute(Qt::WA_Hover, true);
	hide();
}

void DockToast::show_toast(ToastTone tone, const QString &title, const QString &detail, Action action)
{
	if (stopped_)
		return;
	tone_ = tone;
	action_ = std::move(action);
	title_->set_text(title);
	detail_->set_text(detail);
	detail_->setVisible(!detail.isEmpty());
	show_->setVisible(static_cast<bool>(action_));
	reposition();
	raise();
	show();
	update();
	fade_to(1.0);
	// The mouse may already be where the toast appears; then it waits until the mouse leaves.
	if (underMouse())
		countdown_->stop();
	else
		countdown_->start(kShowMs);
}

void DockToast::dismiss()
{
	if (!isVisible())
		return;
	countdown_->stop();
	fade_to(0.0);
}

void DockToast::set_area(const QRect &area)
{
	area_ = area;
	if (isVisible())
		reposition();
}

void DockToast::shutdown()
{
	stopped_ = true;
	countdown_->stop();
	fade_->stop();
	action_ = nullptr;
	hide();
}

void DockToast::paintEvent(QPaintEvent *)
{
	QPainter painter(this);
	painter.setRenderHint(QPainter::Antialiasing, true);
	const QPalette &pal = palette();
	const QRectF card = QRectF(rect()).adjusted(kShadowSide, kShadowTop, -kShadowSide, -kShadowBottom);
	paint_elevated_card(painter, pal, card, kRadius);

	const ThemeAccents &accents = theme_accents();
	const QColor tone = tone_ == ToastTone::success ? accents.success : accents.warning;
	const QFontMetrics metrics(scaled_font(font(), 1.0, QFont::DemiBold));
	const qreal badge = std::round(metrics.height() * kBadge);
	// Level with the first line of the title.
	const QRectF badge_box(card.left() + kPaddingX, card.top() + kPaddingY + (metrics.height() - badge) / 2.0,
			       badge, badge);
	painter.setPen(Qt::NoPen);
	painter.setBrush(with_alpha(tone, 0.2));
	painter.drawEllipse(badge_box);
	const qreal inset = badge * 0.2;
	paint_dock_icon(painter, tone_ == ToastTone::success ? DockIcon::check : DockIcon::warning,
			badge_box.adjusted(inset, inset, -inset, -inset), tone);
}

void DockToast::enterEvent(QEnterEvent *event)
{
	QWidget::enterEvent(event);
	countdown_->stop();
	// Caught while fading out: it comes back and waits.
	if (isVisible() && fade_->endValue().toReal() <= 0.0)
		fade_to(1.0);
}

void DockToast::leaveEvent(QEvent *event)
{
	QWidget::leaveEvent(event);
	if (isVisible() && !stopped_)
		countdown_->start(kResumeMs);
}

void DockToast::changeEvent(QEvent *event)
{
	QWidget::changeEvent(event);
	if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange)
		update();
	if (event->type() == QEvent::FontChange && isVisible())
		reposition();
}

int DockToast::needed_height(int area_width)
{
	const int width = std::min(area_width - 4, kMaxWidth + 2 * kShadowSide);
	return height_for(width);
}

int DockToast::height_for(int width)
{
	// The card's padding, with room on the left for the badge.
	const QFontMetrics metrics(scaled_font(font(), 1.0, QFont::DemiBold));
	const int badge_room = qRound(metrics.height() * kBadge) + 10;
	layout()->setContentsMargins(kShadowSide + kPaddingX + badge_room, kShadowTop + kPaddingY,
				     kShadowSide + kPaddingX - 4, kShadowBottom + kPaddingY);
	return layout()->hasHeightForWidth() ? layout()->totalHeightForWidth(width)
					     : layout()->totalSizeHint().height();
}

void DockToast::reposition()
{
	if (area_.isEmpty())
		return;
	const int width = std::min(area_.width() - 4, kMaxWidth + 2 * kShadowSide);
	const int height = height_for(width);
	const int left = area_.left() + (area_.width() - width) / 2;
	const int top = area_.bottom() + 1 - height;
	setGeometry(left, top, std::max(0, width), height);
}

void DockToast::fade_to(qreal opacity)
{
	const qreal from = opacity_->opacity();
	fade_->stop();
	fade_->setStartValue(from);
	fade_->setEndValue(opacity);
	fade_->setDuration(opacity > from ? kFadeInMs : kFadeOutMs);
	fade_->start();
}

void DockToast::run_action()
{
	// Copied first: the action may show a new toast, which replaces the stored action.
	const Action action = action_;
	dismiss();
	if (action)
		action();
}

} // namespace clip_atlas
