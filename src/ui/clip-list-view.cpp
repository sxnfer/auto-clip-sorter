// Clip Atlas — the dock's clip list view and its delegate. clip-list-view.hpp draws a row and a card; the sizes here
// keep the picture at a fixed size and let the text follow the theme's font.
#include "clip-list-view.hpp"

#include "clip-index.hpp"
#include "clip-list-model.hpp"
#include "dock-style.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <QAbstractItemModel>
#include <QColor>
#include <QCursor>
#include <QEvent>
#include <QFont>
#include <QFontMetrics>
#include <QHelpEvent>
#include <QImage>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QScrollBar>
#include <QSize>
#include <QSizeF>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QTimer>
#include <QToolTip>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <initializer_list>

namespace clip_atlas {

namespace {

// Rows: the rounded fill's inset from the view's sides, the picture's margin inside it, the gap before the text and
// the room above and below the picture.
constexpr int kRowInset = 4;
constexpr int kRowPadding = 8;
constexpr int kTextGap = 10;
constexpr int kRowVerticalPadding = 7;
constexpr qreal kRowRadius = 6.0;
constexpr qreal kPictureRadius = 4.0;
// Cards: the gap around each card and the picture's corners.
constexpr int kCardGap = 10;
constexpr qreal kCardRadius = 6.0;
// The detail line and the time, a little smaller than the title; the New tag and the card's time smaller still.
constexpr qreal kSmallScale = 0.9;
constexpr qreal kTagScale = 0.76;
constexpr qreal kHeaderScale = 0.86;
// An action button's side, in font heights, and the gap between buttons.
constexpr qreal kActionSide = 1.45;
constexpr int kActionGap = 2;

QString module_text(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

DockIcon kind_icon(int kind)
{
	switch (static_cast<ClipKind>(kind)) {
	case ClipKind::replay:
		return DockIcon::replay;
	case ClipKind::recording:
		return DockIcon::recording;
	case ClipKind::screenshot:
		return DockIcon::screenshot;
	}
	return DockIcon::recording;
}

bool keyboard_focus(const QStyleOptionViewItem &option)
{
	return option.state.testFlag(QStyle::State_HasFocus) && option.widget &&
	       option.widget->window()->testAttribute(Qt::WA_KeyboardFocusChange);
}

// Restores the painter when an item is done, including when a draw call throws halfway through, so the next item
// starts from the state the view left.
class PainterState {
public:
	explicit PainterState(QPainter *painter) : painter_(painter) { painter_->save(); }
	~PainterState() { painter_->restore(); }

	PainterState(const PainterState &) = delete;
	PainterState &operator=(const PainterState &) = delete;

private:
	QPainter *painter_;
};

// Draws `picture` cropped to fill `box`, with rounded corners. The corners are cut by an antialiased mask, because a
// clip path's edge is jagged on the raster engine. Scaled in device pixels, so a HiDPI screen shows it sharp.
void draw_cover(QPainter *painter, const QPixmap &picture, const QRectF &box, qreal radius)
{
	const qreal dpr = painter->device() ? painter->device()->devicePixelRatioF() : 1.0;
	const QSize target = (box.size() * dpr).toSize();
	if (target.isEmpty())
		return;
	const QPixmap scaled = picture.scaled(target, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
	QImage tile(target, QImage::Format_ARGB32_Premultiplied);
	tile.fill(Qt::transparent);
	{
		QPainter mask(&tile);
		mask.setRenderHint(QPainter::Antialiasing, true);
		mask.setPen(Qt::NoPen);
		mask.setBrush(QColor(Qt::white));
		mask.drawRoundedRect(QRectF(QPointF(0.0, 0.0), QSizeF(target)), radius * dpr, radius * dpr);
		mask.setCompositionMode(QPainter::CompositionMode_SourceIn);
		mask.drawPixmap(QPointF((target.width() - scaled.width()) / 2.0,
					(target.height() - scaled.height()) / 2.0),
				scaled);
	}
	tile.setDevicePixelRatio(dpr);
	painter->drawImage(box.topLeft(), tile);
}

// A tile in a tint of the base with the kind's icon, for a clip without a picture.
void draw_placeholder(QPainter *painter, const QPalette &palette, const QRectF &box, qreal radius, int kind,
		      qreal icon_side)
{
	painter->setPen(Qt::NoPen);
	painter->setBrush(mix_colors(palette.color(QPalette::Base), palette.color(QPalette::Text), 0.07));
	painter->drawRoundedRect(box, radius, radius);
	const QPointF centre = box.center();
	paint_dock_icon(*painter, kind_icon(kind),
			QRectF(centre.x() - icon_side / 2.0, centre.y() - icon_side / 2.0, icon_side, icon_side),
			muted_text(palette));
}

// The kind's icon in a dark square on the picture's corner, readable on any picture.
void draw_kind_badge(QPainter *painter, const QRectF &box, int kind)
{
	painter->setPen(Qt::NoPen);
	painter->setBrush(QColor(0, 0, 0, 150));
	painter->drawRoundedRect(box, 3.0, 3.0);
	const qreal inset = box.width() * 0.16;
	paint_dock_icon(*painter, kind_icon(kind), box.adjusted(inset, inset, -inset, -inset), QColor(Qt::white));
}

QFont tag_font(const QFont &base)
{
	return scaled_font(base, kTagScale, QFont::DemiBold);
}

QSizeF tag_size(const QFont &base, const QString &text)
{
	const QFontMetricsF metrics(tag_font(base));
	return QSizeF(std::round(metrics.horizontalAdvance(text) + metrics.height() * 0.9),
		      std::round(metrics.height() + 2));
}

// A small pill: Dock.New on the accent, or a card's time on a dark glass.
void draw_tag(QPainter *painter, const QFont &base, const QString &text, const QRectF &box, const QColor &fill,
	      const QColor &color)
{
	painter->setPen(Qt::NoPen);
	painter->setBrush(fill);
	painter->drawRoundedRect(box, box.height() / 2.0, box.height() / 2.0);
	painter->setFont(tag_font(base));
	painter->setPen(color);
	painter->drawText(box, Qt::AlignCenter | Qt::TextSingleLine, text);
}

} // namespace

// --- ClipListDelegate -----------------------------------------------------------------------------------------------

ClipListDelegate::ClipListDelegate(QAbstractItemView *view) : QStyledItemDelegate(view), view_(view)
{
	if (view)
		view->viewport()->installEventFilter(this);
}

void ClipListDelegate::set_style(Style style)
{
	style_ = style;
	set_hover(QModelIndex(), Hit::none);
	emit sizeHintChanged(QModelIndex());
}

bool ClipListDelegate::set_card_height(int height)
{
	height = std::max(height, 60);
	if (height == card_height_)
		return false;
	card_height_ = height;
	return true;
}

int ClipListDelegate::card_width() const
{
	const QFontMetrics metrics(view_ ? view_->font() : QFont());
	const int picture_height = std::max(30, card_height_ - kCardGap - text_block_height(metrics));
	return picture_height * 16 / 9;
}

void ClipListDelegate::set_action_callback(ActionCallback callback)
{
	action_callback_ = std::move(callback);
}

void ClipListDelegate::update_hover(const QPoint &viewport_pos)
{
	if (!view_)
		return;
	const QModelIndex index = view_->indexAt(viewport_pos);
	if (!index.isValid() || index.data(ClipListModel::HeaderRole).toBool()) {
		set_hover(QModelIndex(), Hit::none);
		return;
	}
	set_hover(index, hit_at(view_->visualRect(index), viewport_pos, QFontMetrics(view_->font())));
}

QSize ClipListDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
	const QFontMetrics metrics(option.font);
	if (style_ == Style::cards)
		return QSize(card_width() + kCardGap, card_height_);
	if (index.data(ClipListModel::HeaderRole).toBool())
		return QSize(0, QFontMetrics(scaled_font(option.font, kHeaderScale)).height() + 16);
	const QFontMetrics small(scaled_font(option.font, kSmallScale));
	return QSize(0, std::max(kPictureHeight, metrics.height() + small.height() + 4) + 2 * kRowVerticalPadding);
}

void ClipListDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
	try {
		const PainterState state(painter);
		painter->setRenderHint(QPainter::Antialiasing, true);
		// Intersected rather than replaced, so the view's own clip still applies.
		painter->setClipRect(option.rect, Qt::IntersectClip);
		if (index.data(ClipListModel::HeaderRole).toBool())
			paint_header(painter, option, index);
		else if (style_ == Style::cards)
			paint_card(painter, option, index);
		else
			paint_row(painter, option, index);
	} catch (const std::exception &e) {
		// Qt calls paint() from its event loop. The item is left partly drawn, which is better than a crash.
		obs_log(LOG_WARNING, "clip list: paint() failed: %s", e.what());
	} catch (...) {
		obs_log(LOG_WARNING, "clip list: paint() failed");
	}
}

void ClipListDelegate::paint_header(QPainter *painter, const QStyleOptionViewItem &option,
				    const QModelIndex &index) const
{
	const QPalette &palette = option.palette;
	// The label sits at the bottom, next to the clips it heads, with the room above it parting the groups.
	const QRect box = option.rect.adjusted(kRowInset + kRowPadding, 0, -(kRowInset + kRowPadding), -4);
	const QFont label_font = scaled_font(option.font, kHeaderScale, QFont::DemiBold);
	const QFont detail_font = scaled_font(option.font, kHeaderScale);
	const QFontMetrics label_metrics(label_font);
	const QFontMetrics detail_metrics(detail_font);
	const QString detail = index.data(ClipListModel::DetailRole).toString();
	const int detail_width = detail_metrics.horizontalAdvance(detail);
	const QString label = label_metrics.elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideRight,
						       std::max(0, box.width() - detail_width - 12));
	painter->setFont(label_font);
	painter->setPen(palette.color(QPalette::Text));
	painter->drawText(box, Qt::AlignLeft | Qt::AlignBottom | Qt::TextSingleLine, label);
	painter->setFont(detail_font);
	painter->setPen(muted_text(palette));
	painter->drawText(box, Qt::AlignRight | Qt::AlignBottom | Qt::TextSingleLine, detail);
}

void ClipListDelegate::paint_row(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
	const QPalette &palette = option.palette;
	const QRect item = option.rect;
	const QFontMetrics metrics(option.font);
	const bool selected = option.state.testFlag(QStyle::State_Selected);
	const bool hovered = hover_index_.isValid() && hover_index_ == index;
	const qreal flash = index.data(ClipListModel::FlashRole).toReal();

	// The fill: selection, hover, and the fresh clip's flash over either.
	const QRectF fill_box = QRectF(item).adjusted(kRowInset + 0.5, 1.5, -kRowInset - 0.5, -1.5);
	QColor fill = palette.color(QPalette::Base);
	bool filled = false;
	if (selected) {
		fill = selection_fill(palette);
		filled = true;
	} else if (hovered) {
		fill = hover_fill(palette, QPalette::Base);
		filled = true;
	}
	if (flash > 0.0) {
		fill = mix_colors(fill, palette.color(QPalette::Highlight), 0.6 * flash);
		filled = true;
	}
	if (filled) {
		painter->setPen(Qt::NoPen);
		painter->setBrush(fill);
		painter->drawRoundedRect(fill_box, kRowRadius, kRowRadius);
	}
	if (keyboard_focus(option)) {
		painter->setPen(QPen(accent_text(palette), 1.5));
		painter->setBrush(Qt::NoBrush);
		painter->drawRoundedRect(fill_box.adjusted(0.75, 0.75, -0.75, -0.75), kRowRadius, kRowRadius);
	}

	// The picture, or the kind's tile.
	const QRect picture_box = picture_rect(item, metrics);
	const int kind = index.data(ClipListModel::KindRole).toInt();
	const QPixmap picture = index.data(Qt::DecorationRole).value<QPixmap>();
	if (picture.isNull()) {
		draw_placeholder(painter, palette, picture_box, kPictureRadius, kind, metrics.height());
	} else {
		draw_cover(painter, picture, picture_box, kPictureRadius);
		draw_kind_badge(painter, QRectF(picture_box.left() + 3, picture_box.bottom() - 17, 15, 15), kind);
	}

	// The text block: the title line and the detail line, centred together.
	const QFont small_font = scaled_font(option.font, kSmallScale);
	const QFontMetrics small(small_font);
	const int text_left = picture_box.right() + 1 + kTextGap;
	const int text_right = item.right() - kRowInset - kRowPadding;
	const int block = metrics.height() + 2 + small.height();
	const int top = item.top() + (item.height() - block) / 2;
	const QRect title_line(text_left, top, std::max(0, text_right - text_left + 1), metrics.height());
	const QRect detail_line(text_left, top + metrics.height() + 2, title_line.width(), small.height());

	// The right: the time, or the actions under the mouse.
	int title_room = title_line.width();
	int detail_room = detail_line.width();
	if (hovered) {
		const QRect first = action_rect(item, Hit::open, metrics);
		title_room = detail_room = std::max(0, first.left() - 8 - text_left);
		for (const Hit hit : {Hit::open, Hit::reveal, Hit::more}) {
			const QRectF button = action_rect(item, hit, metrics);
			const bool under = hover_hit_ == hit;
			if (under) {
				painter->setPen(Qt::NoPen);
				painter->setBrush(with_alpha(palette.color(QPalette::Text), 0.12));
				painter->drawRoundedRect(button, 5.0, 5.0);
			}
			const qreal side = std::round(metrics.height() * 0.85);
			const QColor color =
				under ? palette.color(QPalette::Text)
				      : mix_colors(muted_text(palette), palette.color(QPalette::Text), 0.4);
			paint_dock_icon(*painter, icon_of(hit),
					QRectF(button.center().x() - side / 2.0, button.center().y() - side / 2.0, side,
					       side),
					color);
		}
	} else {
		const QString when = index.data(ClipListModel::WhenRole).toString();
		const int when_width = small.horizontalAdvance(when);
		painter->setFont(small_font);
		painter->setPen(muted_text(palette));
		painter->drawText(QRect(title_line.left(), title_line.top(), title_line.width(), title_line.height()),
				  Qt::AlignRight | Qt::AlignVCenter | Qt::TextSingleLine, when);
		title_room = std::max(0, title_room - when_width - 10);
	}

	// The title, then New for the fresh clip.
	const bool fresh = index.data(ClipListModel::FreshRole).toBool();
	const QString tag = fresh ? module_text("Dock.New") : QString();
	const QSizeF tag_box = fresh ? tag_size(option.font, tag) : QSizeF();
	const int tag_room = fresh ? qRound(tag_box.width()) + 8 : 0;
	const QString title = metrics.elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideRight,
						 std::max(0, title_room - tag_room));
	painter->setFont(option.font);
	painter->setPen(palette.color(QPalette::Text));
	painter->drawText(title_line, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine, title);
	if (fresh && title_room >= tag_room) {
		const qreal left = text_left + metrics.horizontalAdvance(title) + 8;
		const QRectF box(left, title_line.center().y() + 0.5 - tag_box.height() / 2.0, tag_box.width(),
				 tag_box.height());
		draw_tag(painter, option.font, tag, box, palette.color(QPalette::Highlight),
			 palette.color(QPalette::HighlightedText));
	}

	painter->setFont(small_font);
	painter->setPen(muted_text(palette));
	painter->drawText(detail_line, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine,
			  small.elidedText(index.data(ClipListModel::DetailRole).toString(), Qt::ElideRight,
					   detail_room));
}

void ClipListDelegate::paint_card(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
	const QPalette &palette = option.palette;
	const QRect item = option.rect;
	const QFontMetrics metrics(option.font);
	const bool selected = option.state.testFlag(QStyle::State_Selected);
	const bool hovered = hover_index_.isValid() && hover_index_ == index;
	const qreal flash = index.data(ClipListModel::FlashRole).toReal();
	const QRectF picture_box = picture_rect(item, metrics);
	const int kind = index.data(ClipListModel::KindRole).toInt();

	const QPixmap picture = index.data(Qt::DecorationRole).value<QPixmap>();
	if (picture.isNull())
		draw_placeholder(painter, palette, picture_box, kCardRadius, kind, metrics.height() * 1.6);
	else
		draw_cover(painter, picture, picture_box, kCardRadius);

	painter->setPen(Qt::NoPen);
	if (hovered) {
		painter->setBrush(QColor(255, 255, 255, 16));
		painter->drawRoundedRect(picture_box, kCardRadius, kCardRadius);
	}
	if (flash > 0.0) {
		painter->setBrush(with_alpha(palette.color(QPalette::Highlight), 0.55 * flash));
		painter->drawRoundedRect(picture_box, kCardRadius, kCardRadius);
	}

	// When, top left, and New, top right, on dark glass and on the accent.
	const qreal inset = 6.0;
	const QString when = index.data(ClipListModel::WhenRole).toString();
	const QSizeF when_size = tag_size(option.font, when);
	const QRectF when_box(picture_box.left() + inset, picture_box.top() + inset,
			      std::min(when_size.width(), picture_box.width() - 2 * inset), when_size.height());
	draw_tag(painter, option.font, when, when_box, QColor(0, 0, 0, 150), QColor(Qt::white));
	if (index.data(ClipListModel::FreshRole).toBool()) {
		const QString tag = module_text("Dock.New");
		const QSizeF tag_box = tag_size(option.font, tag);
		const QRectF box(picture_box.right() - inset - tag_box.width(), picture_box.top() + inset,
				 tag_box.width(), tag_box.height());
		if (box.left() > when_box.right() + 4)
			draw_tag(painter, option.font, tag, box, palette.color(QPalette::Highlight),
				 palette.color(QPalette::HighlightedText));
	}
	if (!picture.isNull()) {
		const qreal badge = std::round(metrics.height() * 1.05);
		draw_kind_badge(painter,
				QRectF(picture_box.left() + inset, picture_box.bottom() - inset - badge, badge, badge),
				kind);
	}
	if (hovered) {
		for (const Hit hit : {Hit::open, Hit::reveal, Hit::more}) {
			const QRectF button = action_rect(item, hit, metrics);
			painter->setPen(Qt::NoPen);
			painter->setBrush(QColor(0, 0, 0, hover_hit_ == hit ? 200 : 140));
			painter->drawEllipse(button);
			const qreal side = std::round(button.width() * 0.56);
			paint_dock_icon(*painter, icon_of(hit),
					QRectF(button.center().x() - side / 2.0, button.center().y() - side / 2.0, side,
					       side),
					QColor(Qt::white));
		}
	}
	if (selected || keyboard_focus(option)) {
		painter->setBrush(Qt::NoBrush);
		painter->setPen(QPen(accent_text(palette), selected ? 2.0 : 1.5));
		painter->drawRoundedRect(picture_box.adjusted(-1.0, -1.0, 1.0, 1.0), kCardRadius + 1.0,
					 kCardRadius + 1.0);
	}

	// The title and the detail under the picture.
	const QFont small_font = scaled_font(option.font, kSmallScale);
	const QFontMetrics small(small_font);
	const int width = qRound(picture_box.width());
	const QRect title_line(qRound(picture_box.left()), qRound(picture_box.bottom()) + 6, width, metrics.height());
	const QRect detail_line(title_line.left(), title_line.bottom() + 1, width, small.height());
	painter->setFont(option.font);
	painter->setPen(palette.color(QPalette::Text));
	painter->drawText(title_line, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine,
			  metrics.elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideRight, width));
	painter->setFont(small_font);
	painter->setPen(muted_text(palette));
	painter->drawText(detail_line, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine,
			  small.elidedText(index.data(ClipListModel::DetailRole).toString(), Qt::ElideRight, width));
}

DockIcon ClipListDelegate::icon_of(Hit hit)
{
	switch (hit) {
	case Hit::open:
		return DockIcon::open;
	case Hit::reveal:
		return DockIcon::folder;
	case Hit::more:
	case Hit::none:
		break;
	}
	return DockIcon::more;
}

QRect ClipListDelegate::picture_rect(const QRect &item, const QFontMetrics &metrics) const
{
	if (style_ == Style::cards) {
		const QRect card = item.adjusted(kCardGap / 2, kCardGap / 2, -kCardGap / 2, -kCardGap / 2);
		return QRect(card.left(), card.top(), card.width(),
			     std::max(30, card.height() - text_block_height(metrics)));
	}
	return QRect(item.left() + kRowInset + kRowPadding, item.top() + (item.height() - kPictureHeight) / 2,
		     kPictureWidth, kPictureHeight);
}

int ClipListDelegate::text_block_height(const QFontMetrics &metrics) const
{
	// A gap, the title and the smaller detail line under a card's picture.
	return 6 + metrics.height() + 1 + qRound(metrics.height() * kSmallScale) + 2;
}

QRect ClipListDelegate::action_rect(const QRect &item, Hit hit, const QFontMetrics &metrics) const
{
	const int side = qRound(metrics.height() * kActionSide);
	// From the right: More, then Show in folder, then Open.
	int from_right = 0;
	if (hit == Hit::reveal)
		from_right = 1;
	else if (hit == Hit::open)
		from_right = 2;
	if (style_ == Style::cards) {
		const QRect picture = picture_rect(item, metrics);
		const int right = picture.right() - 6;
		const int left = right - side + 1 - from_right * (side + kActionGap + 2);
		return QRect(left, picture.bottom() - 6 - side + 1, side, side);
	}
	const int right = item.right() - kRowInset - 6;
	const int left = right - side + 1 - from_right * (side + kActionGap);
	return QRect(left, item.top() + (item.height() - side) / 2, side, side);
}

ClipListDelegate::Hit ClipListDelegate::hit_at(const QRect &item, const QPoint &pos, const QFontMetrics &metrics) const
{
	for (const Hit hit : {Hit::open, Hit::reveal, Hit::more}) {
		if (action_rect(item, hit, metrics).contains(pos))
			return hit;
	}
	return Hit::none;
}

void ClipListDelegate::set_hover(const QModelIndex &index, Hit hit)
{
	if (hover_index_ == index && hover_hit_ == hit)
		return;
	if (view_ && hover_index_.isValid())
		view_->viewport()->update(view_->visualRect(hover_index_));
	hover_index_ = index;
	hover_hit_ = hit;
	if (view_ && hover_index_.isValid())
		view_->viewport()->update(view_->visualRect(hover_index_));
}

bool ClipListDelegate::helpEvent(QHelpEvent *event, QAbstractItemView *view, const QStyleOptionViewItem &option,
				 const QModelIndex &index)
{
	if (event && event->type() == QEvent::ToolTip && index.isValid() &&
	    !index.data(ClipListModel::HeaderRole).toBool()) {
		const QFontMetrics metrics(option.font);
		const Hit hit = hit_at(option.rect, event->pos(), metrics);
		const char *key = nullptr;
		if (hit == Hit::open)
			key = "Dock.Action.Open";
		else if (hit == Hit::reveal)
			key = "Dock.Action.Reveal";
		else if (hit == Hit::more)
			key = "Dock.Action.More";
		if (key) {
			QToolTip::showText(event->globalPos(), module_text(key), view ? view->viewport() : nullptr,
					   action_rect(option.rect, hit, metrics));
			return true;
		}
	}
	return QStyledItemDelegate::helpEvent(event, view, option, index);
}

bool ClipListDelegate::editorEvent(QEvent *event, QAbstractItemModel *model, const QStyleOptionViewItem &option,
				   const QModelIndex &index)
{
	const QEvent::Type type = event->type();
	if (type != QEvent::MouseButtonPress && type != QEvent::MouseButtonDblClick &&
	    type != QEvent::MouseButtonRelease)
		return QStyledItemDelegate::editorEvent(event, model, option, index);
	const auto *mouse = static_cast<QMouseEvent *>(event);
	if (mouse->button() != Qt::LeftButton || index.data(ClipListModel::HeaderRole).toBool())
		return false;
	const QFontMetrics metrics(option.font);
	const Hit hit = hit_at(option.rect, mouse->position().toPoint(), metrics);

	if (type == QEvent::MouseButtonPress) {
		// Not taken: the view still selects the row the action belongs to.
		pressed_index_ = index;
		pressed_hit_ = hit;
		swallow_release_ = false;
		return false;
	}
	if (type == QEvent::MouseButtonDblClick) {
		if (hit == Hit::none)
			return false;
		// Taken, so a quick second click on an action doesn't open the clip as well, nor run the action twice.
		swallow_release_ = true;
		return true;
	}
	const bool same = hit != Hit::none && hit == pressed_hit_ && pressed_index_ == index;
	pressed_index_ = QModelIndex();
	pressed_hit_ = Hit::none;
	if (swallow_release_) {
		swallow_release_ = false;
		return hit != Hit::none;
	}
	if (!same)
		return false;
	// Run after the view has finished with this click: the action may open a menu or a dialog, or change the model.
	const QPersistentModelIndex target(index);
	const QRect button = action_rect(option.rect, hit, metrics);
	const QPoint global = view_ ? view_->viewport()->mapToGlobal(button.bottomLeft()) : QPoint();
	const Action action = hit == Hit::open ? Action::open : hit == Hit::reveal ? Action::reveal : Action::more;
	const ActionCallback callback = action_callback_;
	if (callback && view_) {
		QTimer::singleShot(0, view_, [callback, target, action, global] {
			if (target.isValid())
				callback(target, action, global);
		});
	}
	return true;
}

bool ClipListDelegate::eventFilter(QObject *watched, QEvent *event)
{
	if (view_ && watched == view_->viewport()) {
		switch (event->type()) {
		case QEvent::MouseMove:
			update_hover(static_cast<QMouseEvent *>(event)->position().toPoint());
			break;
		case QEvent::Leave:
			set_hover(QModelIndex(), Hit::none);
			break;
		default:
			break;
		}
	}
	return QStyledItemDelegate::eventFilter(watched, event);
}

// --- ClipListView ---------------------------------------------------------------------------------------------------

ClipListView::ClipListView(QWidget *parent) : QListView(parent)
{
	delegate_ = new ClipListDelegate(this);
	setItemDelegate(delegate_);
	setSelectionMode(QAbstractItemView::SingleSelection);
	// Headers are shorter than clips.
	setUniformItemSizes(false);
	setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
	setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
	setFrameShape(QFrame::NoFrame);
	setContextMenuPolicy(Qt::CustomContextMenu);
	setWrapping(false);
	// The delegate follows the mouse for the hover actions.
	viewport()->setMouseTracking(true);
	set_strip(false);
}

void ClipListView::set_strip(bool strip)
{
	strip_ = strip;
	delegate_->set_style(strip ? ClipListDelegate::Style::cards : ClipListDelegate::Style::rows);
	setFlow(strip ? QListView::LeftToRight : QListView::TopToBottom);
	setHorizontalScrollBarPolicy(strip ? Qt::ScrollBarAsNeeded : Qt::ScrollBarAlwaysOff);
	setVerticalScrollBarPolicy(strip ? Qt::ScrollBarAlwaysOff : Qt::ScrollBarAsNeeded);
	update_card_height();
	scheduleDelayedItemsLayout();
}

QModelIndex ClipListView::moveCursor(CursorAction action, Qt::KeyboardModifiers modifiers)
{
	const QModelIndex next = QListView::moveCursor(action, modifiers);
	QAbstractItemModel *list = model();
	if (!list || !next.isValid() || list->flags(next).testFlag(Qt::ItemIsSelectable))
		return next;
	// A header can't be current. The cursor goes on past it the way it was moving, or back the other way at the end
	// of the list (Page Up onto the first header).
	const bool backwards = action == MoveUp || action == MoveLeft || action == MovePrevious || action == MovePageUp;
	const int rows = list->rowCount(rootIndex());
	for (const int step : {backwards ? -1 : 1, backwards ? 1 : -1}) {
		for (int row = next.row() + step; row >= 0 && row < rows; row += step) {
			const QModelIndex candidate = list->index(row, 0, rootIndex());
			if (!list->flags(candidate).testFlag(Qt::ItemIsSelectable))
				continue;
			// The header above the first clip of a group comes into view with it.
			if (row > 0 &&
			    !list->flags(list->index(row - 1, 0, rootIndex())).testFlag(Qt::ItemIsSelectable))
				scrollTo(list->index(row - 1, 0, rootIndex()), EnsureVisible);
			return candidate;
		}
	}
	return currentIndex();
}

void ClipListView::wheelEvent(QWheelEvent *event)
{
	if (!strip_) {
		QListView::wheelEvent(event);
		return;
	}
	// The strip has no vertical scrolling, so an ordinary wheel scrolls it sideways. A sideways wheel or a touchpad
	// swipe in either direction works too.
	const QPoint pixels = event->pixelDelta();
	const QPoint angle = event->angleDelta();
	int delta = 0;
	if (!pixels.isNull()) {
		delta = std::abs(pixels.x()) > std::abs(pixels.y()) ? pixels.x() : pixels.y();
	} else {
		const int degrees = std::abs(angle.x()) > std::abs(angle.y()) ? angle.x() : angle.y();
		// One notch moves half a card.
		delta = degrees * std::max(1, delegate_->card_width() / 2) / 120;
	}
	QScrollBar *bar = horizontalScrollBar();
	bar->setValue(bar->value() - delta);
	event->accept();
}

void ClipListView::resizeEvent(QResizeEvent *event)
{
	QListView::resizeEvent(event);
	update_card_height();
}

void ClipListView::scrollContentsBy(int dx, int dy)
{
	QListView::scrollContentsBy(dx, dy);
	// The rows moved under a still mouse, so the row under it changed without a mouse event.
	if (viewport()->underMouse())
		delegate_->update_hover(viewport()->mapFromGlobal(QCursor::pos()));
}

void ClipListView::update_card_height()
{
	if (!strip_)
		return;
	// The scroll bar's room is kept whether it shows or not, so the cards don't shrink when it appears, which could
	// make it disappear again.
	const int bar = horizontalScrollBar()->sizeHint().height();
	if (delegate_->set_card_height(std::max(0, contentsRect().height() - bar)))
		scheduleDelayedItemsLayout();
}

} // namespace clip_atlas
