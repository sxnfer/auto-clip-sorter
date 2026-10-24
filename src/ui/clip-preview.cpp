// Clip Atlas — the floating window's preview of the current clip. clip-preview.hpp shows the layout and says how the
// picture is asked for.
#include "clip-preview.hpp"

#include "clip-routing.hpp"
#include "dock-controls.hpp"
#include "dock-style.hpp"
#include "ui-thread.hpp"

#include <obs-module.h>

#include <QEvent>
#include <QGridLayout>
#include <QImage>
#include <QPainter>
#include <QPointF>
#include <QPointer>
#include <QPushButton>
#include <QRectF>
#include <QSize>
#include <QSizeF>
#include <QSizePolicy>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>

namespace clip_atlas {

namespace {

constexpr int kButtonIconPx = 16;
constexpr qreal kPictureRadius = 8.0;

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

TextBlock *fact_label(const char *key, QWidget *parent)
{
	auto *label = new TextBlock(module_text(key), TextTone::muted, 0.92, QFont::Normal, parent);
	label->set_elide(Qt::ElideNone);
	return label;
}

TextBlock *fact_value(QWidget *parent)
{
	auto *value = new TextBlock(QString(), TextTone::normal, 0.92, QFont::Normal, parent);
	value->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
	return value;
}

} // namespace

// The picture, fitted whole inside a 16:9 tile with rounded corners. The tile's tint shows around a picture of another
// shape, and the kind's icon stands in for a clip with no picture.
class PreviewPicture : public QWidget {
public:
	explicit PreviewPicture(QWidget *parent) : QWidget(parent)
	{
		setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	}

	void set_picture(const QPixmap &picture, int kind)
	{
		picture_ = picture;
		kind_ = kind;
		update();
	}

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter painter(this);
		painter.setRenderHint(QPainter::Antialiasing, true);
		// The largest 16:9 box that fits, centred across.
		const qreal box_width = std::min<qreal>(width(), height() * 16.0 / 9.0);
		const qreal box_height = box_width * 9.0 / 16.0;
		const QRectF box((width() - box_width) / 2.0, 0.0, box_width, box_height);
		const QColor tint = mix_colors(palette().color(QPalette::Base), palette().color(QPalette::Text), 0.06);
		if (picture_.isNull()) {
			painter.setPen(Qt::NoPen);
			painter.setBrush(tint);
			painter.drawRoundedRect(box, kPictureRadius, kPictureRadius);
			const qreal side = std::min(box.height() * 0.3, 48.0);
			paint_dock_icon(painter, kind_icon(kind_),
					QRectF(box.center().x() - side / 2.0, box.center().y() - side / 2.0, side,
					       side),
					muted_text(palette()));
			return;
		}
		// Composed off screen and cut by an antialiased mask, because a clip path's edge is jagged on the raster
		// engine.
		const qreal dpr = devicePixelRatioF();
		const QSize target = (box.size() * dpr).toSize();
		if (target.isEmpty())
			return;
		QImage tile(target, QImage::Format_ARGB32_Premultiplied);
		tile.fill(Qt::transparent);
		{
			QPainter compose(&tile);
			compose.setRenderHint(QPainter::Antialiasing, true);
			compose.setPen(Qt::NoPen);
			compose.setBrush(tint);
			compose.drawRoundedRect(QRectF(QPointF(0.0, 0.0), QSizeF(target)), kPictureRadius * dpr,
						kPictureRadius * dpr);
			compose.setCompositionMode(QPainter::CompositionMode_SourceAtop);
			const QPixmap fitted = picture_.scaled(target, Qt::KeepAspectRatio, Qt::SmoothTransformation);
			compose.drawPixmap(QPointF((target.width() - fitted.width()) / 2.0,
						   (target.height() - fitted.height()) / 2.0),
					   fitted);
		}
		tile.setDevicePixelRatio(dpr);
		painter.drawImage(box.topLeft(), tile);
	}

	void changeEvent(QEvent *event) override
	{
		QWidget::changeEvent(event);
		if (event->type() == QEvent::PaletteChange)
			update();
	}

private:
	QPixmap picture_;
	int kind_ = 0;
};

ClipPreview::ClipPreview(QWidget *parent) : QWidget(parent)
{
	nothing_ = new EmptyState(this);
	nothing_->set_texts(DockIcon::open, module_text("Dock.Preview.Select"), QString());

	content_ = new QWidget(this);
	picture_ = new PreviewPicture(content_);
	title_ = new TextBlock(QString(), TextTone::normal, 1.22, QFont::DemiBold, content_);
	title_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
	where_ = new TextBlock(QString(), TextTone::muted, 0.92, QFont::Normal, content_);
	where_->set_wrap(true);

	game_ = fact_value(content_);
	kind_ = fact_value(content_);
	saved_ = fact_value(content_);
	size_ = fact_value(content_);
	auto *facts = new QGridLayout;
	facts->setHorizontalSpacing(16);
	facts->setVerticalSpacing(4);
	facts->setColumnStretch(1, 1);
	const std::pair<const char *, TextBlock *> rows[] = {
		{"Dock.Preview.Game", game_},
		{"Dock.Preview.Kind", kind_},
		{"Dock.Preview.Saved", saved_},
		{"Dock.Preview.Size", size_},
	};
	int row = 0;
	for (const auto &[key, value] : rows) {
		facts->addWidget(fact_label(key, content_), row, 0);
		facts->addWidget(value, row, 1);
		++row;
	}

	open_ = new QPushButton(module_text("Dock.Action.Open"), content_);
	reveal_ = new QPushButton(module_text("Dock.Action.Reveal"), content_);
	rename_ = new QPushButton(module_text("Dock.Action.Rename"), content_);
	move_ = new QPushButton(module_text("Dock.Action.MoveToGame"), content_);
	recycle_ = new QPushButton(module_text("Dock.Action.Recycle"), content_);
	update_icons();
	auto *buttons = new QGridLayout;
	buttons->setSpacing(6);
	buttons->addWidget(open_, 0, 0);
	buttons->addWidget(reveal_, 0, 1);
	buttons->addWidget(rename_, 1, 0);
	buttons->addWidget(move_, 1, 1);
	buttons->addWidget(recycle_, 2, 0, 1, 2);
	const std::pair<QPushButton *, PreviewAction> actions[] = {
		{open_, PreviewAction::open}, {reveal_, PreviewAction::reveal},   {rename_, PreviewAction::rename},
		{move_, PreviewAction::move}, {recycle_, PreviewAction::recycle},
	};
	for (const auto &[button, action] : actions) {
		const PreviewAction chosen = action;
		connect(button, &QPushButton::clicked, this, [this, chosen] { run(chosen); });
	}

	auto *column = new QVBoxLayout(content_);
	column->setContentsMargins(0, 0, 0, 0);
	column->setSpacing(6);
	column->addWidget(picture_);
	column->addSpacing(6);
	column->addWidget(title_);
	column->addWidget(where_);
	column->addSpacing(8);
	column->addLayout(facts);
	column->addSpacing(12);
	column->addLayout(buttons);
	column->addStretch(1);

	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(14, 12, 14, 12);
	root->addWidget(content_);
	root->addWidget(nothing_);
	show_nothing();
}

ClipPreview::~ClipPreview()
{
	shutdown();
}

void ClipPreview::show_clip(const PreviewInfo &info)
{
	const bool same = has_clip_ && cache_key(info) == cache_key(info_);
	info_ = info;
	has_clip_ = true;
	title_->set_text(info.title);
	title_->setToolTip(info.title);
	where_->set_text(info.where);
	game_->set_text(info.game);
	kind_->set_text(info.kind_text);
	saved_->set_text(info.saved);
	size_->set_text(info.size_text);
	nothing_->hide();
	content_->show();
	// The same file again (a rescan, a new filter): the picture stays and nothing is asked for.
	if (same && has_large_picture_)
		return;
	if (const QPixmap *cached = cache_.get(cache_key(info))) {
		has_large_picture_ = true;
		picture_->set_picture(*cached, info.kind);
		return;
	}
	has_large_picture_ = false;
	picture_->set_picture(info.fallback, info.kind);
	request_picture(0);
}

void ClipPreview::show_nothing()
{
	has_clip_ = false;
	has_large_picture_ = false;
	info_ = PreviewInfo{};
	// A newer ticket turns away the answer to a request still on its way.
	++ticket_;
	content_->hide();
	nothing_->show();
}

void ClipPreview::set_fallback(const std::string &path_utf8, const QPixmap &picture)
{
	if (!has_clip_ || has_large_picture_ || picture.isNull() || path_utf8 != info_.path_utf8)
		return;
	info_.fallback = picture;
	picture_->set_picture(picture, info_.kind);
}

void ClipPreview::set_action_callback(ActionCallback callback)
{
	action_callback_ = std::move(callback);
}

void ClipPreview::shutdown()
{
	stopped_ = true;
	++ticket_;
}

void ClipPreview::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	fit_picture();
}

void ClipPreview::changeEvent(QEvent *event)
{
	QWidget::changeEvent(event);
	// The base class can send events before the children exist.
	if (!open_)
		return;
	if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange)
		update_icons();
	if (event->type() == QEvent::FontChange)
		fit_picture();
}

std::string ClipPreview::cache_key(const PreviewInfo &info) const
{
	return info.path_utf8 + '\n' + std::to_string(info.size) + '\n' + std::to_string(info.modified_ms);
}

void ClipPreview::request_picture(int attempt)
{
	if (stopped_ || !has_clip_)
		return;
	const std::uint64_t ticket = ++ticket_;
	const QPointer<ClipPreview> self(this);
	auto answered = [self, ticket, attempt](const std::string &, ThumbnailStatus status, Thumbnail thumb) {
		// run_on_ui() needs a copyable callable, so the pixels go in a shared_ptr instead of being copied.
		auto picture = std::make_shared<Thumbnail>(std::move(thumb));
		run_on_ui([self, ticket, attempt, status, picture]() {
			if (self)
				self->on_picture(ticket, attempt, status, *picture);
		});
	};
	thumbnail_provider().request(info_.path_utf8, kPicturePx, std::move(answered));
}

void ClipPreview::on_picture(std::uint64_t ticket, int attempt, ThumbnailStatus status, const Thumbnail &thumb)
{
	// Another clip was shown since, or the preview stopped: the answer is for something no longer on screen.
	if (stopped_ || ticket != ticket_ || !has_clip_)
		return;
	switch (status) {
	case ThumbnailStatus::ok: {
		const std::size_t needed = static_cast<std::size_t>(std::max(thumb.width, 0)) *
					   static_cast<std::size_t>(std::max(thumb.height, 0)) * 4;
		if (thumb.empty() || thumb.bgra.size() < needed)
			return;
		const QImage view(thumb.bgra.data(), thumb.width, thumb.height, thumb.width * 4,
				  QImage::Format_ARGB32_Premultiplied);
		// copy(): the picture must own its pixels, since `thumb` goes away when this returns.
		const QPixmap picture = QPixmap::fromImage(view.copy());
		cache_.put(cache_key(info_), picture);
		has_large_picture_ = true;
		picture_->set_picture(picture, info_.kind);
		break;
	}
	case ThumbnailStatus::failed:
		// The list's picture or the kind's tile stays.
		break;
	case ThumbnailStatus::dropped:
		if (attempt == 0) {
			const QPointer<ClipPreview> self(this);
			const std::uint64_t waiting = ticket_;
			QTimer::singleShot(kRetryMs, this, [self, waiting] {
				if (self && self->ticket_ == waiting)
					self->request_picture(1);
			});
		}
		break;
	}
}

void ClipPreview::update_icons()
{
	open_->setIcon(dock_icon(DockIcon::open, palette(), kButtonIconPx));
	reveal_->setIcon(dock_icon(DockIcon::folder, palette(), kButtonIconPx));
	recycle_->setIcon(dock_icon(DockIcon::trash, palette(), kButtonIconPx));
}

void ClipPreview::fit_picture()
{
	// 16:9 of the width, but never more than half the height, so a short window keeps the facts and the buttons.
	const QRect room = contentsRect();
	const int margins = 28;
	const int height = std::max(48, std::min((room.width() - margins) * 9 / 16, room.height() / 2));
	picture_->setFixedHeight(height);
}

void ClipPreview::run(PreviewAction action)
{
	if (!has_clip_)
		return;
	// Copied first: the action may open a dialog whose event loop changes the preview.
	const ActionCallback callback = action_callback_;
	if (callback)
		callback(action);
}

} // namespace clip_atlas
