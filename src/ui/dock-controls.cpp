// Clip Atlas — the dock's painted controls. dock-controls.hpp says why they paint themselves. Sizes are fractions of the
// font's height rather than pixels, so the controls grow with the theme's font and with the display's scaling.
#include "dock-controls.hpp"

#include <obs-module.h>

#include <QAction>
#include <QEvent>
#include <QFontMetrics>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QLayout>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPen>
#include <QPointF>
#include <QPointer>
#include <QRect>
#include <QRectF>
#include <QSize>
#include <QSizePolicy>
#include <QStyle>
#include <QStyleOption>
#include <QVariant>
#include <QVariantAnimation>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numeric>
#include <utility>

namespace clip_atlas {

namespace {

// Chips: their height, the room either side of the label and between the label and the count, in font heights.
constexpr qreal kChipHeight = 1.5;
constexpr qreal kChipPadding = 0.62;
constexpr qreal kChipGap = 0.38;
constexpr qreal kCountScale = 0.9;
// Between chips, across and down.
constexpr qreal kChipSpacing = 0.34;

// The type filter's track and the room either side of a segment's label.
constexpr qreal kTrackHeight = 1.6;
constexpr qreal kTrackInset = 2.0; // px between the track and its segments
constexpr qreal kSegmentPadding = 0.6;

// The switch's track and the gap between the label and the track.
constexpr qreal kSwitchHeight = 1.05;
constexpr qreal kSwitchAspect = 1.8;
constexpr qreal kSwitchGap = 0.5;

constexpr qreal kAvatarSide = 2.1;
constexpr qreal kPillHeight = 1.45;
constexpr qreal kGhostSide = 1.6;

// Painted text this much smaller than the theme's font: the pill, counts and secondary lines.
constexpr qreal kSmallScale = 0.92;

QString module_text(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

int font_height(const QWidget *widget)
{
	return QFontMetrics(widget->font()).height();
}

// The colour of a control's label: the palette's text, or the muted grey while it is disabled.
QColor label_color(const QWidget *widget, QPalette::ColorRole role = QPalette::Text)
{
	return widget->isEnabled() ? widget->palette().color(role) : muted_text(widget->palette());
}

bool under_mouse(const QWidget *widget)
{
	return widget->isEnabled() && widget->testAttribute(Qt::WA_UnderMouse);
}

// The roles every control repaints for, and the font change after which its size is asked for again.
void handle_look_change(QWidget *widget, QEvent *event)
{
	switch (event->type()) {
	case QEvent::FontChange:
		widget->updateGeometry();
		widget->update();
		break;
	case QEvent::PaletteChange:
	case QEvent::StyleChange:
	case QEvent::EnabledChange:
		widget->update();
		break;
	default:
		break;
	}
}

} // namespace

void paint_focus_ring(QPainter &painter, const QWidget *widget, const QRectF &rect, qreal radius)
{
	QStyleOption option;
	option.initFrom(widget);
	if (!option.state.testFlag(QStyle::State_HasFocus) || !option.state.testFlag(QStyle::State_KeyboardFocusChange))
		return;
	painter.save();
	painter.setRenderHint(QPainter::Antialiasing, true);
	painter.setPen(QPen(accent_text(widget->palette()), 2.0));
	painter.setBrush(Qt::NoBrush);
	painter.drawRoundedRect(rect.adjusted(1.0, 1.0, -1.0, -1.0), radius, radius);
	painter.restore();
}

// --- ChipButton -----------------------------------------------------------------------------------------------------

ChipButton::ChipButton(QWidget *parent) : QAbstractButton(parent)
{
	setAttribute(Qt::WA_Hover, true);
	setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

void ChipButton::set_count(int count)
{
	if (count_ == count)
		return;
	count_ = count;
	updateGeometry();
	update();
}

QSize ChipButton::sizeHint() const
{
	const QFontMetrics metrics(font());
	const QFontMetrics count_metrics(scaled_font(font(), kCountScale));
	int width = 2 * qRound(metrics.height() * kChipPadding) + metrics.horizontalAdvance(text());
	if (count_ >= 0)
		width += qRound(metrics.height() * kChipGap) + count_metrics.horizontalAdvance(QString::number(count_));
	return QSize(width, qRound(metrics.height() * kChipHeight));
}

QSize ChipButton::minimumSizeHint() const
{
	const int height = qRound(font_height(this) * kChipHeight);
	return QSize(height * 2, height);
}

void ChipButton::paintEvent(QPaintEvent *)
{
	QPainter painter(this);
	painter.setRenderHint(QPainter::Antialiasing, true);
	const QPalette &pal = palette();
	const QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
	const qreal radius = box.height() / 2.0;
	const bool checked = isChecked();

	QColor fill = pal.color(QPalette::Button);
	if (checked)
		fill = pal.color(QPalette::Highlight);
	else if (isDown())
		fill = mix_colors(fill, pal.color(QPalette::ButtonText), 0.16);
	else if (under_mouse(this))
		fill = hover_fill(pal, QPalette::Button);
	if (!isEnabled())
		fill = with_alpha(fill, 0.5);
	painter.setPen(Qt::NoPen);
	painter.setBrush(fill);
	painter.drawRoundedRect(box, radius, radius);

	const QFontMetrics metrics(font());
	const QFont count_font = scaled_font(font(), kCountScale);
	const QFontMetrics count_metrics(count_font);
	const int padding = qRound(metrics.height() * kChipPadding);
	const QString count = count_ >= 0 ? QString::number(count_) : QString();
	const int count_room =
		count.isEmpty() ? 0 : qRound(metrics.height() * kChipGap) + count_metrics.horizontalAdvance(count);
	const QRect text_box = rect().adjusted(padding, 0, -padding, 0);
	const QString label = metrics.elidedText(text(), Qt::ElideRight, std::max(0, text_box.width() - count_room));

	const QColor text_color = checked ? pal.color(QPalette::HighlightedText)
					  : label_color(this, QPalette::ButtonText);
	painter.setPen(text_color);
	painter.setFont(font());
	painter.drawText(text_box, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine, label);
	if (!count.isEmpty()) {
		const int label_width = metrics.horizontalAdvance(label);
		QRect count_box = text_box;
		count_box.setLeft(text_box.left() + label_width + qRound(metrics.height() * kChipGap));
		painter.setPen(checked ? with_alpha(text_color, 0.78) : muted_text(pal));
		painter.setFont(count_font);
		painter.drawText(count_box, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine, count);
	}
	paint_focus_ring(painter, this, box, radius);
}

void ChipButton::changeEvent(QEvent *event)
{
	QAbstractButton::changeEvent(event);
	handle_look_change(this, event);
}

// --- ChipBar --------------------------------------------------------------------------------------------------------

ChipBar::ChipBar(QWidget *parent) : QWidget(parent)
{
	QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Preferred);
	policy.setHeightForWidth(true);
	setSizePolicy(policy);

	// Not checkable, so the chips' exclusive group leaves it alone.
	more_ = new ChipButton(this);
	more_->setToolTip(module_text("Dock.Filter.More.Tooltip"));
	more_->setAccessibleName(module_text("Dock.Filter.More.Tooltip"));
	more_->setFocusPolicy(Qt::ClickFocus);
	more_->installEventFilter(this);
	more_->hide();
	connect(more_, &QAbstractButton::clicked, this, [this] { open_more_menu(); });
}

void ChipBar::set_entries(const std::vector<Entry> &entries, int current)
{
	while (chips_.size() > entries.size()) {
		ChipButton *chip = chips_.back();
		chips_.pop_back();
		chip->hide();
		// Later, not now: this can run from the chip's own click.
		chip->deleteLater();
	}
	while (chips_.size() < entries.size()) {
		const int index = static_cast<int>(chips_.size());
		auto *chip = new ChipButton(this);
		chip->setCheckable(true);
		// The chips share this parent, so checking one unchecks the rest, and clicking the checked chip keeps it.
		chip->setAutoExclusive(true);
		chip->installEventFilter(this);
		connect(chip, &QAbstractButton::clicked, this, [this, index] { choose(index); });
		chips_.push_back(chip);
	}
	for (std::size_t i = 0; i < entries.size(); ++i) {
		chips_[i]->setText(entries[i].label);
		chips_[i]->set_count(entries[i].count);
		chips_[i]->updateGeometry();
	}
	current_ = chips_.empty() ? 0 : std::clamp(current, 0, static_cast<int>(chips_.size()) - 1);
	if (!chips_.empty())
		chips_[static_cast<std::size_t>(current_)]->setChecked(true);
	update_focus_chain();
	updateGeometry();
	relayout();
}

void ChipBar::set_current(int index)
{
	if (chips_.empty())
		return;
	current_ = std::clamp(index, 0, static_cast<int>(chips_.size()) - 1);
	chips_[static_cast<std::size_t>(current_)]->setChecked(true);
	update_focus_chain();
	// The checked chip may have been in the menu, and it is always shown.
	relayout();
}

void ChipBar::set_chosen_callback(Chosen callback)
{
	chosen_ = std::move(callback);
}

void ChipBar::set_max_rows(int rows)
{
	rows = std::max(1, rows);
	if (rows == max_rows_)
		return;
	max_rows_ = rows;
	updateGeometry();
	relayout();
}

bool ChipBar::hasHeightForWidth() const
{
	return true;
}

int ChipBar::heightForWidth(int width) const
{
	const int rows = chips_.empty() ? 1 : place(width).rows;
	return rows * chip_height() + (rows - 1) * spacing();
}

QSize ChipBar::sizeHint() const
{
	// Every chip on one line, up to a width that would make the bar crowd out the search field in the bottom row.
	int width = 0;
	for (const ChipButton *chip : chips_)
		width += chip->sizeHint().width() + spacing();
	width = std::clamp(width, chip_height() * 3, chip_height() * 24);
	return QSize(width, heightForWidth(width));
}

QSize ChipBar::minimumSizeHint() const
{
	// Narrower than any chip: a long game name is elided rather than widening the dock.
	return QSize(chip_height() * 3, chip_height());
}

void ChipBar::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	relayout();
}

void ChipBar::changeEvent(QEvent *event)
{
	QWidget::changeEvent(event);
	if (event->type() == QEvent::FontChange) {
		updateGeometry();
		relayout();
	}
}

bool ChipBar::eventFilter(QObject *watched, QEvent *event)
{
	if (event->type() == QEvent::KeyPress) {
		const int key = static_cast<QKeyEvent *>(event)->key();
		if (key == Qt::Key_Left || key == Qt::Key_Right) {
			// The chips in screen order, then "+N" when it is shown.
			std::vector<QWidget *> stops;
			for (int index : shown_)
				stops.push_back(chips_[static_cast<std::size_t>(index)]);
			if (more_->isVisible())
				stops.push_back(more_);
			const auto at = std::find(stops.begin(), stops.end(), watched);
			if (at != stops.end()) {
				const auto position = static_cast<int>(at - stops.begin());
				const int next = std::clamp(position + (key == Qt::Key_Right ? 1 : -1), 0,
							    static_cast<int>(stops.size()) - 1);
				stops[static_cast<std::size_t>(next)]->setFocus(Qt::TabFocusReason);
				return true;
			}
		}
	}
	return QWidget::eventFilter(watched, event);
}

int ChipBar::chip_height() const
{
	return qRound(font_height(this) * kChipHeight);
}

int ChipBar::spacing() const
{
	return std::max(4, qRound(font_height(this) * kChipSpacing));
}

int ChipBar::more_width(int hidden) const
{
	const QFontMetrics metrics(font());
	return 2 * qRound(metrics.height() * kChipPadding) +
	       metrics.horizontalAdvance(module_text("Dock.Filter.More").arg(hidden));
}

ChipBar::Placement ChipBar::place_in_order(int width, const std::vector<int> &order) const
{
	Placement placement;
	const int room = std::max(1, width);
	const int height = chip_height();
	const int gap = spacing();
	int x = 0;
	int row = 0;
	std::size_t next = 0;
	for (; next < order.size(); ++next) {
		const ChipButton *chip = chips_[static_cast<std::size_t>(order[next])];
		const int chip_width = std::min(chip->sizeHint().width(), room);
		if (x > 0 && x + chip_width > room) {
			if (row + 1 >= max_rows_)
				break;
			++row;
			x = 0;
		}
		placement.shown.push_back(order[next]);
		placement.rects.emplace_back(x, row * (height + gap), chip_width, height);
		x += chip_width + gap;
	}
	placement.rows = row + 1;
	if (next == order.size())
		return placement;

	// The rest go in the menu. "+N" ends the last row, so chips come off that row until it fits.
	int hidden = static_cast<int>(order.size() - next);
	const int last_row_top = row * (height + gap);
	while (true) {
		const int width_needed = std::min(more_width(hidden), room);
		const bool row_has_chips = !placement.rects.empty() && placement.rects.back().top() == last_row_top;
		const int more_left = row_has_chips ? placement.rects.back().right() + 1 + gap : 0;
		if (!row_has_chips || more_left + width_needed <= room) {
			placement.more = QRect(more_left, last_row_top, width_needed, height);
			break;
		}
		placement.rects.pop_back();
		placement.shown.pop_back();
		++hidden;
	}
	placement.hidden = hidden;
	return placement;
}

ChipBar::Placement ChipBar::place(int width) const
{
	std::vector<int> order(chips_.size());
	std::iota(order.begin(), order.end(), 0);
	const Placement first = place_in_order(width, order);
	const bool current_shown = std::find(first.shown.begin(), first.shown.end(), current_) != first.shown.end();
	if (first.hidden == 0 || current_shown)
		return first;
	// The checked chip would be in the menu. It moves in front of the last chip shown, then further forward, until it
	// fits; the chips it passes keep their order.
	for (int position = static_cast<int>(first.shown.size()) - 1; position >= 0; --position) {
		std::vector<int> moved = order;
		moved.erase(std::find(moved.begin(), moved.end(), current_));
		moved.insert(moved.begin() + position, current_);
		Placement placement = place_in_order(width, moved);
		if (std::find(placement.shown.begin(), placement.shown.end(), current_) != placement.shown.end())
			return placement;
	}
	return first;
}

void ChipBar::relayout()
{
	const Placement placement = place(width());
	std::vector<bool> on_screen(chips_.size(), false);
	for (std::size_t i = 0; i < placement.shown.size(); ++i) {
		ChipButton *chip = chips_[static_cast<std::size_t>(placement.shown[i])];
		chip->setGeometry(placement.rects[i]);
		chip->show();
		on_screen[static_cast<std::size_t>(placement.shown[i])] = true;
	}
	shown_ = placement.shown;
	hidden_.clear();
	for (std::size_t i = 0; i < chips_.size(); ++i) {
		if (!on_screen[i]) {
			chips_[i]->hide();
			hidden_.push_back(static_cast<int>(i));
		}
	}
	if (placement.hidden > 0) {
		more_->setText(module_text("Dock.Filter.More").arg(placement.hidden));
		more_->setGeometry(placement.more);
		more_->show();
	} else {
		more_->hide();
	}
}

void ChipBar::update_focus_chain()
{
	// One Tab stop, the checked chip; the arrows reach the others.
	for (std::size_t i = 0; i < chips_.size(); ++i)
		chips_[i]->setFocusPolicy(static_cast<int>(i) == current_ ? Qt::StrongFocus : Qt::ClickFocus);
}

void ChipBar::choose(int index)
{
	if (index < 0 || index >= static_cast<int>(chips_.size()))
		return;
	set_current(index);
	// Copied first: the callback may replace itself.
	const Chosen callback = chosen_;
	if (callback)
		callback(index);
}

void ChipBar::open_more_menu()
{
	// No parent, so the menu is destroyed when this returns and never by a parent deleted while it is open.
	QMenu menu;
	for (int index : hidden_) {
		const ChipButton *chip = chips_[static_cast<std::size_t>(index)];
		QString label = chip->text();
		if (chip->count() >= 0)
			label += QStringLiteral("  ") + QString::number(chip->count());
		QAction *action = menu.addAction(label);
		action->setData(index);
	}
	const QPointer<ChipBar> self(this);
	QAction *chosen = menu.exec(more_->mapToGlobal(QPoint(0, more_->height())));
	// The menu's event loop may have deleted the bar.
	if (!self || !chosen)
		return;
	self->choose(chosen->data().toInt());
}

// --- SegmentButton and SegmentBar -----------------------------------------------------------------------------------

SegmentButton::SegmentButton(const QString &label, bool has_icon, DockIcon icon, QWidget *parent)
	: QAbstractButton(parent),
	  has_icon_(has_icon),
	  icon_(icon)
{
	setText(label);
	setAccessibleName(label);
	setCheckable(true);
	setAutoExclusive(true);
	setAttribute(Qt::WA_Hover, true);
}

void SegmentButton::set_compact(bool compact)
{
	const bool shown_compact = compact && has_icon_;
	if (compact_ == shown_compact)
		return;
	compact_ = shown_compact;
	// A label that isn't shown is still said by the tooltip.
	setToolTip(compact_ ? text() : QString());
	update();
}

int SegmentButton::full_width() const
{
	const QFontMetrics metrics(font());
	return 2 * qRound(metrics.height() * kSegmentPadding) + metrics.horizontalAdvance(text());
}

int SegmentButton::compact_width() const
{
	if (!has_icon_)
		return full_width();
	const QFontMetrics metrics(font());
	return 2 * qRound(metrics.height() * kSegmentPadding) + metrics.height();
}

QSize SegmentButton::sizeHint() const
{
	return QSize(full_width(), qRound(font_height(this) * kTrackHeight - 2 * kTrackInset));
}

QSize SegmentButton::minimumSizeHint() const
{
	return QSize(compact_width(), sizeHint().height());
}

void SegmentButton::paintEvent(QPaintEvent *)
{
	QPainter painter(this);
	painter.setRenderHint(QPainter::Antialiasing, true);
	const QPalette &pal = palette();
	const QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
	const qreal radius = std::min(5.0, box.height() / 3.0);
	const bool light = is_light_palette(pal);

	if (isChecked()) {
		// A raised thumb: the input grey on a dark theme, the base with an outline on a light one.
		painter.setPen(light ? QPen(line_color(pal), 1.0) : QPen(Qt::NoPen));
		painter.setBrush(light ? pal.color(QPalette::Base) : pal.color(QPalette::Button));
		painter.drawRoundedRect(box, radius, radius);
	} else if (under_mouse(this)) {
		painter.setPen(Qt::NoPen);
		painter.setBrush(with_alpha(pal.color(QPalette::Text), 0.06));
		painter.drawRoundedRect(box, radius, radius);
	}

	const QColor color = isChecked() || under_mouse(this) ? label_color(this) : muted_text(pal);
	if (compact_) {
		const qreal side = std::min<qreal>(QFontMetricsF(font()).height(), box.height() - 4.0);
		const QRectF icon_box(box.center().x() - side / 2.0, box.center().y() - side / 2.0, side, side);
		paint_dock_icon(painter, icon_, icon_box, color);
	} else {
		const QFontMetrics metrics(font());
		const int padding = qRound(metrics.height() * kSegmentPadding);
		const QRect text_box = rect().adjusted(padding / 2, 0, -padding / 2, 0);
		painter.setPen(color);
		painter.setFont(font());
		painter.drawText(text_box, Qt::AlignCenter | Qt::TextSingleLine,
				 metrics.elidedText(text(), Qt::ElideRight, text_box.width()));
	}
	paint_focus_ring(painter, this, box, radius);
}

void SegmentButton::changeEvent(QEvent *event)
{
	QAbstractButton::changeEvent(event);
	handle_look_change(this, event);
}

SegmentBar::SegmentBar(const std::vector<Segment> &segments, QWidget *parent) : QWidget(parent)
{
	setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
	for (const Segment &segment : segments) {
		const int index = static_cast<int>(segments_.size());
		auto *button = new SegmentButton(segment.label, segment.has_icon, segment.icon, this);
		button->installEventFilter(this);
		connect(button, &QAbstractButton::clicked, this, [this, index] { choose(index); });
		segments_.push_back(button);
	}
	set_current(0);
}

void SegmentBar::set_current(int index)
{
	if (segments_.empty())
		return;
	current_ = std::clamp(index, 0, static_cast<int>(segments_.size()) - 1);
	segments_[static_cast<std::size_t>(current_)]->setChecked(true);
	// One Tab stop, the checked segment, as with the chips.
	for (std::size_t i = 0; i < segments_.size(); ++i)
		segments_[i]->setFocusPolicy(static_cast<int>(i) == current_ ? Qt::StrongFocus : Qt::ClickFocus);
}

void SegmentBar::set_chosen_callback(Chosen callback)
{
	chosen_ = std::move(callback);
}

QSize SegmentBar::sizeHint() const
{
	int width = 0;
	for (const SegmentButton *segment : segments_)
		width += segment->full_width();
	return QSize(width + qRound(2 * kTrackInset), qRound(font_height(this) * kTrackHeight));
}

QSize SegmentBar::minimumSizeHint() const
{
	int width = 0;
	for (const SegmentButton *segment : segments_)
		width += segment->compact_width();
	return QSize(width + qRound(2 * kTrackInset), qRound(font_height(this) * kTrackHeight));
}

void SegmentBar::paintEvent(QPaintEvent *)
{
	QPainter painter(this);
	painter.setRenderHint(QPainter::Antialiasing, true);
	const QPalette &pal = palette();
	const QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
	const qreal radius = std::min(7.0, box.height() / 3.0);
	painter.setPen(Qt::NoPen);
	painter.setBrush(
		mix_colors(pal.color(QPalette::Base), pal.color(QPalette::Text), is_light_palette(pal) ? 0.07 : 0.05));
	painter.drawRoundedRect(box, radius, radius);
}

void SegmentBar::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	relayout();
}

void SegmentBar::changeEvent(QEvent *event)
{
	QWidget::changeEvent(event);
	handle_look_change(this, event);
	if (event->type() == QEvent::FontChange)
		relayout();
}

bool SegmentBar::eventFilter(QObject *watched, QEvent *event)
{
	if (event->type() == QEvent::KeyPress) {
		const int key = static_cast<QKeyEvent *>(event)->key();
		const auto at = std::find(segments_.begin(), segments_.end(), watched);
		if (at != segments_.end() && (key == Qt::Key_Left || key == Qt::Key_Right)) {
			const auto position = static_cast<int>(at - segments_.begin());
			const int next = std::clamp(position + (key == Qt::Key_Right ? 1 : -1), 0,
						    static_cast<int>(segments_.size()) - 1);
			if (next != position) {
				choose(next);
				segments_[static_cast<std::size_t>(next)]->setFocus(Qt::TabFocusReason);
			}
			return true;
		}
	}
	return QWidget::eventFilter(watched, event);
}

void SegmentBar::relayout()
{
	if (segments_.empty())
		return;
	const int inset = qRound(kTrackInset);
	const QRect inner = rect().adjusted(inset, inset, -inset, -inset);
	int full = 0;
	for (const SegmentButton *segment : segments_)
		full += segment->full_width();
	const bool compact = full > inner.width();
	// With labels, each segment gets its label's width and an equal share of what is left. Compact segments share the
	// width equally.
	const int count = static_cast<int>(segments_.size());
	std::vector<int> widths(segments_.size());
	int used = 0;
	for (int i = 0; i < count; ++i) {
		SegmentButton *segment = segments_[static_cast<std::size_t>(i)];
		segment->set_compact(compact);
		const int base = compact ? 0 : segment->full_width();
		widths[static_cast<std::size_t>(i)] = base;
		used += base;
	}
	const int spare = std::max(0, inner.width() - used);
	int x = inner.left();
	for (int i = 0; i < count; ++i) {
		// The last segment takes the rounding remainder, so the segments meet the track's right edge.
		int width = widths[static_cast<std::size_t>(i)] + spare / count;
		if (i == count - 1)
			width = inner.right() + 1 - x;
		segments_[static_cast<std::size_t>(i)]->setGeometry(x, inner.top(), std::max(0, width), inner.height());
		x += width;
	}
}

void SegmentBar::choose(int index)
{
	set_current(index);
	const Chosen callback = chosen_;
	if (callback)
		callback(current_);
}

// --- ToggleSwitch ---------------------------------------------------------------------------------------------------

ToggleSwitch::ToggleSwitch(const QString &text, QWidget *parent) : QAbstractButton(parent)
{
	setText(text);
	setCheckable(true);
	setAttribute(Qt::WA_Hover, true);
	setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
	slide_ = new QVariantAnimation(this);
	slide_->setDuration(140);
	slide_->setEasingCurve(QEasingCurve::OutCubic);
	connect(slide_, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
		knob_ = value.toReal();
		update();
	});
	connect(this, &QAbstractButton::toggled, this, [this](bool on) {
		const qreal target = on ? 1.0 : 0.0;
		// From where the knob is: part way while it slides, otherwise the other end.
		const qreal from = slide_->state() == QAbstractAnimation::Running ? knob_ : 1.0 - target;
		slide_->stop();
		// A hidden switch has nothing to animate. It paints from its check state when it shows.
		if (!isVisible()) {
			update();
			return;
		}
		slide_->setStartValue(from);
		slide_->setEndValue(target);
		slide_->start();
	});
}

QSize ToggleSwitch::sizeHint() const
{
	const QFontMetrics metrics(font());
	const int track_height = qRound(metrics.height() * kSwitchHeight);
	const int track_width = qRound(track_height * kSwitchAspect);
	int width = track_width + 4; // the focus ring's room
	if (!text().isEmpty())
		width += metrics.horizontalAdvance(text()) + qRound(metrics.height() * kSwitchGap);
	return QSize(width, std::max(metrics.height(), track_height) + 4);
}

QSize ToggleSwitch::minimumSizeHint() const
{
	return sizeHint();
}

void ToggleSwitch::paintEvent(QPaintEvent *)
{
	QPainter painter(this);
	painter.setRenderHint(QPainter::Antialiasing, true);
	const QPalette &pal = palette();
	const QFontMetrics metrics(font());
	const qreal track_height = std::round(metrics.height() * kSwitchHeight);
	const qreal track_width = std::round(track_height * kSwitchAspect);
	const QRectF track(width() - 2.0 - track_width, (height() - track_height) / 2.0, track_width, track_height);

	if (!text().isEmpty()) {
		const QRect text_box(0, 0, qRound(track.left()) - qRound(metrics.height() * kSwitchGap), height());
		painter.setPen(label_color(this));
		painter.setFont(font());
		painter.drawText(text_box, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine, text());
	}

	const bool light = is_light_palette(pal);
	QColor off = mix_colors(pal.color(QPalette::Base), pal.color(QPalette::Text), light ? 0.22 : 0.26);
	QColor on = pal.color(QPalette::Highlight);
	if (under_mouse(this)) {
		off = mix_colors(off, pal.color(QPalette::Text), 0.1);
		on = mix_colors(on, QColor(Qt::white), 0.1);
	}
	// The animation's position while it runs, otherwise the check state: setChecked() with the signals blocked moves
	// no animation, and the knob must still be in place.
	const qreal knob = slide_->state() == QAbstractAnimation::Running ? knob_ : (isChecked() ? 1.0 : 0.0);
	QColor fill = mix_colors(off, on, knob);
	if (!isEnabled())
		fill = with_alpha(fill, 0.45);
	const qreal radius = track_height / 2.0;
	painter.setPen(Qt::NoPen);
	painter.setBrush(fill);
	painter.drawRoundedRect(track, radius, radius);

	const qreal inset = 2.0;
	const qreal knob_side = track_height - 2 * inset;
	const qreal knob_left = track.left() + inset + knob * (track_width - track_height);
	const QColor knob_off = light ? pal.color(QPalette::Base) : mix_colors(pal.color(QPalette::Text), off, 0.12);
	const QColor knob_color = mix_colors(knob_off, pal.color(QPalette::HighlightedText), knob);
	painter.setBrush(isEnabled() ? knob_color : with_alpha(knob_color, 0.6));
	painter.drawEllipse(QRectF(knob_left, track.top() + inset, knob_side, knob_side));

	paint_focus_ring(painter, this, track.adjusted(-2.0, -2.0, 2.0, 2.0), radius + 2.0);
}

void ToggleSwitch::changeEvent(QEvent *event)
{
	QAbstractButton::changeEvent(event);
	handle_look_change(this, event);
}

// --- GameAvatar -----------------------------------------------------------------------------------------------------

GameAvatar::GameAvatar(QWidget *parent) : QWidget(parent)
{
	setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

void GameAvatar::set_game(const QString &name)
{
	const QString initials = initials_of(name);
	const bool has_game = !name.isEmpty();
	if (initials == initials_ && has_game == has_game_)
		return;
	initials_ = initials;
	has_game_ = has_game;
	update();
}

QSize GameAvatar::sizeHint() const
{
	const int side = qRound(font_height(this) * kAvatarSide);
	return QSize(side, side);
}

QSize GameAvatar::minimumSizeHint() const
{
	return sizeHint();
}

void GameAvatar::paintEvent(QPaintEvent *)
{
	QPainter painter(this);
	painter.setRenderHint(QPainter::Antialiasing, true);
	const QPalette &pal = palette();
	const qreal side = std::min(width(), height());
	const QRectF box((width() - side) / 2.0, (height() - side) / 2.0, side, side);
	const qreal radius = side * 0.26;
	painter.setPen(Qt::NoPen);
	if (has_game_ && !initials_.isEmpty()) {
		painter.setBrush(pal.color(QPalette::Highlight));
		painter.drawRoundedRect(box, radius, radius);
		// Two wide letters at the full size can touch the edges, so the font shrinks to leave a margin.
		QFont letters = scaled_font(font(), 1.08, QFont::DemiBold);
		const qreal room = side * 0.72;
		const qreal advance = QFontMetricsF(letters).horizontalAdvance(initials_);
		if (advance > room && advance > 0)
			letters = scaled_font(letters, room / advance, QFont::DemiBold);
		painter.setFont(letters);
		painter.setPen(pal.color(QPalette::HighlightedText));
		painter.drawText(box, Qt::AlignCenter | Qt::TextSingleLine, initials_);
		return;
	}
	painter.setBrush(mix_colors(pal.color(QPalette::Base), pal.color(QPalette::Text), 0.08));
	painter.drawRoundedRect(box, radius, radius);
	const qreal icon_side = side * 0.56;
	paint_dock_icon(painter, DockIcon::gamepad,
			QRectF(box.center().x() - icon_side / 2.0, box.center().y() - icon_side / 2.0, icon_side,
			       icon_side),
			muted_text(pal));
}

void GameAvatar::changeEvent(QEvent *event)
{
	QWidget::changeEvent(event);
	handle_look_change(this, event);
}

// --- GameTitle ------------------------------------------------------------------------------------------------------

GameTitle::GameTitle(QWidget *parent) : QWidget(parent)
{
	// Ignored across: a long game name must not ask for room. It gets what is left and is elided to it.
	setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
}

void GameTitle::set_texts(const QString &eyebrow, const QString &title, bool dimmed)
{
	eyebrow_ = eyebrow;
	title_ = title;
	dimmed_ = dimmed;
	update_tooltip();
	update();
}

QSize GameTitle::sizeHint() const
{
	const QFontMetrics eyebrow_metrics(eyebrow_font());
	const QFontMetrics title_metrics(title_font());
	const int width =
		std::max(eyebrow_metrics.horizontalAdvance(eyebrow_), title_metrics.horizontalAdvance(title_));
	return QSize(width, eyebrow_metrics.height() + title_metrics.height());
}

QSize GameTitle::minimumSizeHint() const
{
	return QSize(0, sizeHint().height());
}

void GameTitle::paintEvent(QPaintEvent *)
{
	QPainter painter(this);
	const QPalette &pal = palette();
	const QFont eyebrow = eyebrow_font();
	const QFont title = title_font();
	const QFontMetrics eyebrow_metrics(eyebrow);
	const QFontMetrics title_metrics(title);
	// The two lines sit together in the middle of whatever height the layout gives.
	const int block = eyebrow_metrics.height() + title_metrics.height();
	const int top = std::max(0, (height() - block) / 2);
	const QRect eyebrow_box(0, top, width(), eyebrow_metrics.height());
	const QRect title_box(0, top + eyebrow_metrics.height(), width(), title_metrics.height());

	painter.setFont(eyebrow);
	painter.setPen(muted_text(pal));
	painter.drawText(eyebrow_box, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine,
			 eyebrow_metrics.elidedText(eyebrow_, Qt::ElideRight, width()));
	painter.setFont(title);
	painter.setPen(dimmed_ ? muted_text(pal) : pal.color(QPalette::Text));
	painter.drawText(title_box, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine,
			 title_metrics.elidedText(title_, Qt::ElideRight, width()));
}

void GameTitle::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	update_tooltip();
}

void GameTitle::changeEvent(QEvent *event)
{
	QWidget::changeEvent(event);
	handle_look_change(this, event);
	if (event->type() == QEvent::FontChange)
		update_tooltip();
}

QFont GameTitle::eyebrow_font() const
{
	return scaled_font(font(), 0.86);
}

QFont GameTitle::title_font() const
{
	return scaled_font(font(), 1.18, QFont::DemiBold);
}

void GameTitle::update_tooltip()
{
	const bool elided = QFontMetrics(title_font()).horizontalAdvance(title_) > width();
	setToolTip(elided && !dimmed_ ? title_ : QString());
}

// --- StatusPill -----------------------------------------------------------------------------------------------------

StatusPill::StatusPill(QWidget *parent) : QWidget(parent)
{
	setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
}

void StatusPill::set_status(const QColor &dot, const QString &text, const QString &detail)
{
	dot_ = dot;
	text_ = text;
	detail_ = detail;
	updateGeometry();
	update();
}

QSize StatusPill::sizeHint() const
{
	const QFontMetrics base(font());
	const QFontMetrics metrics(scaled_font(font(), kSmallScale));
	const int padding = qRound(base.height() * 0.6);
	const int dot_room = qRound(base.height() * 0.42) + qRound(base.height() * 0.4);
	int width = 2 * padding + dot_room + metrics.horizontalAdvance(text_);
	if (!detail_.isEmpty())
		width += metrics.horizontalAdvance(QStringLiteral(" · ") + detail_);
	return QSize(width, qRound(base.height() * kPillHeight));
}

QSize StatusPill::minimumSizeHint() const
{
	const QFontMetrics base(font());
	return QSize(qRound(base.height() * 4.0), qRound(base.height() * kPillHeight));
}

void StatusPill::paintEvent(QPaintEvent *)
{
	QPainter painter(this);
	painter.setRenderHint(QPainter::Antialiasing, true);
	const QPalette &pal = palette();
	const QFontMetrics base(font());
	const QFont small = scaled_font(font(), kSmallScale);
	const QFontMetrics metrics(small);

	// The pill hugs its text: the layout may give more width than the text needs.
	const int wanted = sizeHint().width();
	const qreal pill_width = std::min(width(), wanted);
	const qreal pill_height = std::min<qreal>(height(), std::round(base.height() * kPillHeight));
	const QRectF pill(0.5, (height() - pill_height) / 2.0 + 0.5, pill_width - 1.0, pill_height - 1.0);
	painter.setPen(Qt::NoPen);
	painter.setBrush(notice_fill(pal));
	painter.drawRoundedRect(pill, pill.height() / 2.0, pill.height() / 2.0);

	const int padding = qRound(base.height() * 0.6);
	const qreal dot = std::round(base.height() * 0.42);
	painter.setBrush(dot_.isValid() ? dot_ : muted_text(pal));
	painter.drawEllipse(QRectF(padding, pill.center().y() - dot / 2.0, dot, dot));

	const int text_left = padding + qRound(dot) + qRound(base.height() * 0.4);
	const int room = std::max(0, qRound(pill.right()) - padding - text_left);
	const QString separated = QStringLiteral(" · ") + detail_;
	const bool detail_fits = !detail_.isEmpty() &&
				 metrics.horizontalAdvance(text_) + metrics.horizontalAdvance(separated) <= room;
	const QRect text_box(text_left, 0, room, height());
	painter.setFont(small);
	painter.setPen(label_color(this));
	const QString status = metrics.elidedText(text_, Qt::ElideRight, room);
	painter.drawText(text_box, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine, status);
	if (detail_fits) {
		QRect detail_box = text_box;
		detail_box.setLeft(text_left + metrics.horizontalAdvance(text_));
		painter.setPen(muted_text(pal));
		painter.drawText(detail_box, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine, separated);
	}
}

void StatusPill::changeEvent(QEvent *event)
{
	QWidget::changeEvent(event);
	handle_look_change(this, event);
}

// --- NoticeFrame ----------------------------------------------------------------------------------------------------

NoticeFrame::NoticeFrame(NoticeTone tone, QWidget *parent) : QFrame(parent), tone_(tone)
{
	// The frame paints itself, so the theme's frame style must not draw a second border.
	setFrameShape(QFrame::NoFrame);
}

void NoticeFrame::set_tone(NoticeTone tone)
{
	tone_ = tone;
	update();
}

int NoticeFrame::icon_room() const
{
	const int height = font_height(this);
	return qRound(height * 0.65) + height + qRound(height * 0.5);
}

void NoticeFrame::paintEvent(QPaintEvent *)
{
	QPainter painter(this);
	painter.setRenderHint(QPainter::Antialiasing, true);
	const QPalette &pal = palette();
	const QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
	painter.setPen(QPen(line_color(pal), 1.0));
	painter.setBrush(notice_fill(pal));
	painter.drawRoundedRect(box, 6.0, 6.0);

	const int height = font_height(this);
	const QColor color = tone_ == NoticeTone::warning ? theme_accents().warning : accent_text(pal);
	// Level with the first line of text, which starts at the layout's top margin.
	const int top = contentsMargins().top() + (layout() ? layout()->contentsMargins().top() : 0);
	paint_dock_icon(painter, tone_ == NoticeTone::warning ? DockIcon::warning : DockIcon::check,
			QRectF(qRound(height * 0.65), top, height, height), color);
}

void NoticeFrame::changeEvent(QEvent *event)
{
	QFrame::changeEvent(event);
	handle_look_change(this, event);
}

// --- HairLine -------------------------------------------------------------------------------------------------------

HairLine::HairLine(Qt::Orientation orientation, QWidget *parent) : QWidget(parent), orientation_(orientation)
{
	if (orientation_ == Qt::Horizontal) {
		setFixedHeight(1);
		setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	} else {
		setFixedWidth(1);
		setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
	}
}

QSize HairLine::sizeHint() const
{
	return QSize(1, 1);
}

void HairLine::paintEvent(QPaintEvent *)
{
	QPainter painter(this);
	painter.fillRect(rect(), line_color(palette()));
}

// --- GhostButton ----------------------------------------------------------------------------------------------------

GhostButton::GhostButton(DockIcon icon, const QString &tooltip, QWidget *parent)
	: QAbstractButton(parent),
	  has_icon_(true),
	  icon_(icon)
{
	setToolTip(tooltip);
	setAccessibleName(tooltip);
	setAttribute(Qt::WA_Hover, true);
	setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

GhostButton::GhostButton(const QString &text, QWidget *parent) : QAbstractButton(parent), has_icon_(false)
{
	setText(text);
	setAttribute(Qt::WA_Hover, true);
	setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

void GhostButton::set_icon(DockIcon icon)
{
	icon_ = icon;
	has_icon_ = true;
	update();
}

QSize GhostButton::sizeHint() const
{
	const QFontMetrics metrics(font());
	const int side = qRound(metrics.height() * kGhostSide);
	if (has_icon_)
		return QSize(side, side);
	const QFontMetrics bold(scaled_font(font(), 1.0, QFont::DemiBold));
	return QSize(bold.horizontalAdvance(text()) + 2 * qRound(metrics.height() * 0.5), side);
}

QSize GhostButton::minimumSizeHint() const
{
	return sizeHint();
}

void GhostButton::paintEvent(QPaintEvent *)
{
	QPainter painter(this);
	painter.setRenderHint(QPainter::Antialiasing, true);
	const QPalette &pal = palette();
	const QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
	const qreal radius = 5.0;
	// Translucent, so the fill suits whatever the button sits on: the dock, a notice or the toast.
	if (isEnabled() && (isDown() || under_mouse(this))) {
		painter.setPen(Qt::NoPen);
		painter.setBrush(with_alpha(pal.color(QPalette::Text), isDown() ? 0.16 : 0.09));
		painter.drawRoundedRect(box, radius, radius);
	}
	if (has_icon_) {
		QColor color = mix_colors(muted_text(pal), pal.color(QPalette::Text), 0.45);
		if (under_mouse(this))
			color = pal.color(QPalette::Text);
		if (!isEnabled())
			color = with_alpha(muted_text(pal), 0.55);
		const qreal side = std::round(QFontMetricsF(font()).height() * 0.95);
		paint_dock_icon(painter, icon_,
				QRectF(box.center().x() - side / 2.0, box.center().y() - side / 2.0, side, side),
				color);
	} else {
		painter.setFont(scaled_font(font(), 1.0, QFont::DemiBold));
		painter.setPen(isEnabled() ? accent_text(pal) : muted_text(pal));
		painter.drawText(rect(), Qt::AlignCenter | Qt::TextSingleLine, text());
	}
	paint_focus_ring(painter, this, box, radius);
}

void GhostButton::changeEvent(QEvent *event)
{
	QAbstractButton::changeEvent(event);
	handle_look_change(this, event);
}

// --- TextBlock ------------------------------------------------------------------------------------------------------

TextBlock::TextBlock(QWidget *parent) : QWidget(parent)
{
	setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
}

TextBlock::TextBlock(const QString &text, TextTone tone, qreal scale, QFont::Weight weight, QWidget *parent)
	: TextBlock(parent)
{
	text_ = text;
	tone_ = tone;
	scale_ = scale;
	weight_ = weight;
}

void TextBlock::set_text(const QString &text)
{
	if (text == text_)
		return;
	text_ = text;
	changed();
}

void TextBlock::set_tone(TextTone tone)
{
	tone_ = tone;
	update();
}

void TextBlock::set_scale(qreal scale, QFont::Weight weight)
{
	scale_ = scale;
	weight_ = weight;
	changed();
}

void TextBlock::set_wrap(bool wrap)
{
	wrap_ = wrap;
	QSizePolicy policy = sizePolicy();
	policy.setHeightForWidth(wrap);
	policy.setVerticalPolicy(wrap ? QSizePolicy::Preferred : QSizePolicy::Fixed);
	setSizePolicy(policy);
	changed();
}

void TextBlock::set_elide(Qt::TextElideMode mode)
{
	elide_ = mode;
	changed();
}

void TextBlock::set_alignment(Qt::Alignment alignment)
{
	alignment_ = alignment;
	update();
}

bool TextBlock::hasHeightForWidth() const
{
	return wrap_;
}

int TextBlock::heightForWidth(int width) const
{
	const QFontMetrics metrics(text_font());
	if (!wrap_ || text_.isEmpty())
		return metrics.height();
	return metrics.boundingRect(QRect(0, 0, std::max(1, width), 100000), Qt::TextWordWrap | Qt::AlignLeft, text_)
		.height();
}

QSize TextBlock::sizeHint() const
{
	const QFontMetrics metrics(text_font());
	const int width = metrics.horizontalAdvance(text_);
	if (!wrap_)
		return QSize(width, metrics.height());
	// A wrapped block asks for a comfortable line length; the layout's width decides the rest.
	const int line = std::min(width, metrics.averageCharWidth() * 48);
	return QSize(line, heightForWidth(line));
}

QSize TextBlock::minimumSizeHint() const
{
	const QFontMetrics metrics(text_font());
	if (wrap_)
		return QSize(metrics.averageCharWidth() * 6, metrics.height());
	if (elide_ == Qt::ElideNone)
		return QSize(metrics.horizontalAdvance(text_), metrics.height());
	return QSize(0, metrics.height());
}

void TextBlock::paintEvent(QPaintEvent *)
{
	if (text_.isEmpty())
		return;
	QPainter painter(this);
	const QPalette &pal = palette();
	const QFont font = text_font();
	const QFontMetrics metrics(font);
	QColor color = pal.color(QPalette::Text);
	if (tone_ == TextTone::muted || !isEnabled())
		color = muted_text(pal);
	else if (tone_ == TextTone::accent)
		color = accent_text(pal);
	painter.setFont(font);
	painter.setPen(color);
	if (wrap_) {
		painter.drawText(rect(), static_cast<int>(alignment_) | Qt::TextWordWrap, text_);
		return;
	}
	const QString shown = elide_ == Qt::ElideNone ? text_ : metrics.elidedText(text_, elide_, width());
	painter.drawText(rect(), static_cast<int>(alignment_) | Qt::TextSingleLine, shown);
}

void TextBlock::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	update_tooltip();
}

void TextBlock::changeEvent(QEvent *event)
{
	QWidget::changeEvent(event);
	handle_look_change(this, event);
	if (event->type() == QEvent::FontChange)
		update_tooltip();
}

QFont TextBlock::text_font() const
{
	return scaled_font(font(), scale_, weight_);
}

void TextBlock::changed()
{
	update_tooltip();
	updateGeometry();
	update();
}

void TextBlock::update_tooltip()
{
	const bool elided = !wrap_ && elide_ != Qt::ElideNone &&
			    QFontMetrics(text_font()).horizontalAdvance(text_) > width();
	setToolTip(elided ? text_ : QString());
}

// --- EmptyState -----------------------------------------------------------------------------------------------------

EmptyState::EmptyState(QWidget *parent) : QWidget(parent)
{
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void EmptyState::set_texts(DockIcon icon, const QString &title, const QString &body)
{
	icon_ = icon;
	title_ = title;
	body_ = body;
	update();
}

QSize EmptyState::sizeHint() const
{
	const int height = font_height(this);
	return QSize(height * 14, height * 9);
}

QSize EmptyState::minimumSizeHint() const
{
	const int height = font_height(this);
	return QSize(height * 6, height * 4);
}

void EmptyState::paintEvent(QPaintEvent *)
{
	QPainter painter(this);
	painter.setRenderHint(QPainter::Antialiasing, true);
	const QPalette &pal = palette();
	const QFont title_font = scaled_font(font(), 1.1, QFont::DemiBold);
	const QFontMetrics title_metrics(title_font);
	const QFontMetrics body_metrics(font());
	const int line = body_metrics.height();
	const int icon_side = qRound(line * 2.2);
	const int text_width = std::min(width() - 2 * line, body_metrics.averageCharWidth() * 40);
	const QRect body_bounds = body_metrics.boundingRect(QRect(0, 0, std::max(1, text_width), 100000),
							    Qt::TextWordWrap | Qt::AlignHCenter, body_);
	const int gap = qRound(line * 0.6);
	const int block = icon_side + gap + title_metrics.height() + qRound(line * 0.25) + body_bounds.height();
	int top = std::max(0, (height() - block) / 2);

	const QColor muted = muted_text(pal);
	paint_dock_icon(painter, icon_, QRectF((width() - icon_side) / 2.0, top, icon_side, icon_side),
			with_alpha(muted, 0.85));
	top += icon_side + gap;
	painter.setFont(title_font);
	painter.setPen(pal.color(QPalette::Text));
	painter.drawText(QRect(line, top, std::max(0, width() - 2 * line), title_metrics.height()),
			 Qt::AlignHCenter | Qt::AlignVCenter | Qt::TextSingleLine,
			 title_metrics.elidedText(title_, Qt::ElideRight, std::max(0, width() - 2 * line)));
	top += title_metrics.height() + qRound(line * 0.25);
	painter.setFont(font());
	painter.setPen(muted);
	painter.drawText(QRect((width() - text_width) / 2, top, std::max(1, text_width), body_bounds.height()),
			 Qt::TextWordWrap | Qt::AlignHCenter, body_);
}

void EmptyState::changeEvent(QEvent *event)
{
	QWidget::changeEvent(event);
	handle_look_change(this, event);
}

} // namespace clip_atlas
