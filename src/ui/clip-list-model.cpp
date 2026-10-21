// Clip Atlas — the dock's clip list: the model over ClipIndex, and the delegate that draws each row.
// clip-list-model.hpp says what each one shows; this file keeps the thumbnail cache and the pending requests in step
// with the rows.
#include "clip-list-model.hpp"

#include "clip-layout.hpp"
#include "ui-thread.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <QApplication>
#include <QColor>
#include <QDateTime>
#include <QFont>
#include <QFontMetrics>
#include <QIcon>
#include <QImage>
#include <QPainter>
#include <QPalette>
#include <QPointF>
#include <QPointer>
#include <QRect>
#include <QRectF>
#include <QSize>
#include <QSizeF>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QWidget>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <exception>
#include <memory>
#include <utility>

namespace clip_atlas {

namespace {

// Space around the thumbnail, and between the thumbnail and the text.
constexpr int kMargin = 4;
constexpr int kGap = 8;
// Between the file name line and the details line.
constexpr int kLineGap = 2;
// Small next to the thumbnail area, so a clip without a picture reads as "no picture yet", not as a broken one.
constexpr int kPlaceholderPx = 24;
// The kind label is a little smaller than the file name.
constexpr double kKindScale = 0.9;

QString module_text(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

// The core's strings are UTF-8 std::strings. The explicit length keeps an embedded NUL from cutting the text short.
QString utf8(const std::string &text)
{
	return QString::fromUtf8(text.data(), static_cast<int>(text.size()));
}

// system_clock counts from the Unix epoch: C++20 requires it, and MSVC and libstdc++ already do in C++17. Milliseconds
// is the precision QDateTime keeps, so a clip's modified time survives the trip to the delegate.
QDateTime to_date_time(std::chrono::system_clock::time_point t)
{
	const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count();
	return QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(ms));
}

std::chrono::system_clock::time_point to_time_point(const QDateTime &t)
{
	return std::chrono::system_clock::time_point(std::chrono::milliseconds(t.toMSecsSinceEpoch()));
}

// "2 min ago", or the date for a clip a week old or more. describe_age() gives the count; the wording is the locale's.
QString age_text(std::chrono::system_clock::time_point modified, std::chrono::system_clock::time_point now)
{
	const Age age = describe_age(modified, now);
	switch (age.unit) {
	case AgeUnit::just_now:
		return module_text("Dock.Age.JustNow");
	case AgeUnit::minutes:
		return module_text("Dock.Age.Minutes").arg(age.count);
	case AgeUnit::hours:
		return module_text("Dock.Age.Hours").arg(age.count);
	case AgeUnit::days:
		return module_text("Dock.Age.Days").arg(age.count);
	case AgeUnit::date:
		break;
	}
	return utf8(format_local_date(modified));
}

QString kind_text(int kind)
{
	switch (static_cast<ClipKind>(kind)) {
	case ClipKind::replay:
		return module_text("Dock.Kind.Replay");
	case ClipKind::recording:
		return module_text("Dock.Kind.Recording");
	case ClipKind::screenshot:
		return module_text("Dock.Kind.Screenshot");
	}
	return QString();
}

// Restores the painter when a row is done, including when a draw call throws halfway through, so the next row
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

} // namespace

ClipListModel::ClipListModel(QObject *parent) : QAbstractListModel(parent) {}

ClipListModel::~ClipListModel()
{
	shutdown();
}

void ClipListModel::reset_clips(std::vector<ClipEntry> clips)
{
	beginResetModel();
	// A rescan (Refresh, a settings change) must not blank the list, so a picture survives it
	// while its file is the same: the size and modified time upsert_clip() compares. Any other
	// picture is dropped. Failed marks and pending requests are dropped as a whole: a failed clip
	// gets one more try per scan, and an answer still on its way is ignored, because its ticket is
	// no longer the pending one.
	std::map<std::filesystem::path, const ClipEntry *> before;
	for (const ClipEntry &old : index_.clips())
		before.emplace(old.path, &old);
	for (const ClipEntry &clip : clips) {
		const auto old = before.find(clip.path);
		const bool same_file = old != before.end() && old->second->size == clip.size &&
				       old->second->modified == clip.modified;
		if (!same_file)
			thumbnails_.erase(path_to_utf8(clip.path));
	}
	failed_.clear();
	pending_.clear();
	index_.reset(std::move(clips));
	rebuild_rows();
	endResetModel();
}

void ClipListModel::upsert_clip(ClipEntry clip)
{
	// A reset rather than row-by-row inserts: one change can move a row, or push another out past
	// the cap, and the dock reselects by path anyway.
	beginResetModel();
	const ClipEntry *old = index_.find(clip.path);
	// The picture belongs to the file as it was. A replaced file (new size or time) loses it, and so
	// does a request still in flight, which would answer with the old file. A path new to the index
	// starts clean too, in case a clip the cap pushed out left a picture behind.
	if (old == nullptr || old->size != clip.size || old->modified != clip.modified)
		forget_thumbnail(path_to_utf8(clip.path));
	index_.upsert(std::move(clip));
	rebuild_rows();
	endResetModel();
}

void ClipListModel::remove_clip(const std::filesystem::path &path)
{
	beginResetModel();
	forget_thumbnail(path_to_utf8(path));
	index_.remove(path);
	rebuild_rows();
	endResetModel();
}

void ClipListModel::set_query(ClipQuery query)
{
	// Pictures are keyed by path, not by row, so a filter change keeps them all.
	beginResetModel();
	query_ = std::move(query);
	rebuild_rows();
	endResetModel();
}

const ClipQuery &ClipListModel::query() const
{
	return query_;
}

const ClipIndex &ClipListModel::index() const
{
	return index_;
}

const ClipEntry *ClipListModel::clip_at(int row) const
{
	if (row < 0 || row >= static_cast<int>(rows_.size()))
		return nullptr;
	return &rows_[static_cast<std::size_t>(row)];
}

int ClipListModel::row_of(const std::filesystem::path &path) const
{
	for (std::size_t i = 0; i < rows_.size(); ++i) {
		if (rows_[i].path == path)
			return static_cast<int>(i);
	}
	return -1;
}

void ClipListModel::refresh_ages()
{
	if (rows_.empty())
		return;
	// No roles given: every role may have changed, and the age text is worked out from the modified
	// time on every paint.
	emit dataChanged(QAbstractListModel::index(0, 0), QAbstractListModel::index(rowCount() - 1, 0));
}

void ClipListModel::shutdown()
{
	// Answers still on the way find no pending request and are ignored; stopped_ turns away new ones.
	stopped_ = true;
	pending_.clear();
}

int ClipListModel::rowCount(const QModelIndex &parent) const
{
	// A flat list: only the root has rows.
	return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant ClipListModel::data(const QModelIndex &index, int role) const
{
	try {
		if (!index.isValid() || index.row() >= static_cast<int>(rows_.size()))
			return QVariant();
		const ClipEntry &clip = rows_[static_cast<std::size_t>(index.row())];
		switch (role) {
		case Qt::DisplayRole:
			return utf8(clip.file_name);
		case Qt::ToolTipRole:
			return utf8(path_to_utf8(clip.path));
		case Qt::DecorationRole: {
			const std::string key = path_to_utf8(clip.path);
			if (QPixmap *picture = thumbnails_.get(key))
				return *picture;
			// A miss asks once: the path stays pending until the answer comes. A failed clip
			// is not asked again until it changes, so scrolling past it doesn't ask the Shell.
			if (!stopped_ && pending_.count(key) == 0 && failed_.count(key) == 0)
				request_thumbnail(key);
			return QPixmap();
		}
		case PathRole:
			return utf8(path_to_utf8(clip.path));
		case KindRole:
			return static_cast<int>(clip.kind);
		case GameRole:
			return utf8(clip.game_folder);
		case SizeRole:
			return static_cast<qulonglong>(clip.size);
		case ModifiedRole:
			return to_date_time(clip.modified);
		default:
			return QVariant();
		}
	} catch (const std::exception &e) {
		// Qt calls data() from its event loop, so nothing may leave it. The cell falls back to "no data".
		obs_log(LOG_WARNING, "clip list: data() failed: %s", e.what());
		return QVariant();
	} catch (...) {
		obs_log(LOG_WARNING, "clip list: data() failed");
		return QVariant();
	}
}

void ClipListModel::rebuild_rows()
{
	rows_ = index_.query(query_);
}

void ClipListModel::request_thumbnail(const std::string &path_utf8) const
{
	const std::uint64_t ticket = ++next_ticket_;
	pending_[path_utf8] = ticket;
	// The answer arrives on the provider's thread and is queued to the UI thread, where the QPointer
	// tells whether the model still exists: a dock closed while a request was out must not be touched.
	QPointer<ClipListModel> self(const_cast<ClipListModel *>(this));
	auto answered = [self, ticket](const std::string &path, ThumbnailStatus status, Thumbnail thumb) {
		// run_on_ui() needs a copyable callable, so the pixels go in a shared_ptr instead of being copied.
		auto picture = std::make_shared<Thumbnail>(std::move(thumb));
		run_on_ui([self, path, ticket, status, picture]() {
			if (self)
				self->on_thumbnail(path, ticket, status, *picture);
		});
	};
	thumbnail_provider().request(path_utf8, kThumbRequestPx, std::move(answered));
}

void ClipListModel::on_thumbnail(const std::string &path_utf8, std::uint64_t ticket, ThumbnailStatus status,
				 const Thumbnail &thumb)
{
	if (stopped_)
		return;
	const auto pending = pending_.find(path_utf8);
	if (pending == pending_.end() || pending->second != ticket)
		return;
	pending_.erase(pending);

	switch (status) {
	case ThumbnailStatus::ok: {
		// A picture whose pixels fall short of its size would be read past its end, so it counts as no picture.
		const std::size_t needed = static_cast<std::size_t>(std::max(thumb.width, 0)) *
					   static_cast<std::size_t>(std::max(thumb.height, 0)) * 4;
		if (thumb.empty() || thumb.bgra.size() < needed) {
			failed_.insert(path_utf8);
			return;
		}
		const QImage view(thumb.bgra.data(), thumb.width, thumb.height, thumb.width * 4,
				  QImage::Format_ARGB32_Premultiplied);
		// copy(): the picture must own its pixels, since `thumb` goes away when this returns.
		thumbnails_.put(path_utf8, QPixmap::fromImage(view.copy()));
		break;
	}
	case ThumbnailStatus::failed:
		// The placeholder stays, so there is nothing to repaint.
		failed_.insert(path_utf8);
		return;
	case ThumbnailStatus::dropped:
		// The next paint asks again.
		return;
	}

	// The row may have been filtered out or removed since the request. Then the picture waits in the cache for it.
	const int row = row_of(path_from_utf8(path_utf8));
	if (row >= 0)
		emit dataChanged(QAbstractListModel::index(row, 0), QAbstractListModel::index(row, 0),
				 {Qt::DecorationRole});
}

void ClipListModel::forget_thumbnail(const std::string &path_utf8)
{
	thumbnails_.erase(path_utf8);
	pending_.erase(path_utf8);
	failed_.erase(path_utf8);
}

ClipListDelegate::ClipListDelegate(QObject *parent) : QStyledItemDelegate(parent) {}

QSize ClipListDelegate::sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const
{
	// Width 0: QListView then gives each row the viewport's width, with or without a scrollbar.
	return QSize(0, ClipListModel::kRowHeight);
}

void ClipListDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
	try {
		// initStyleOption() fills in the state, palette and font the view chose. Text and icon are
		// drawn below, so they are cleared here; PE_PanelItemViewItem paints just the background.
		QStyleOptionViewItem opt(option);
		initStyleOption(&opt, index);
		opt.text.clear();
		opt.icon = QIcon();
		const QWidget *widget = opt.widget;
		QStyle *style = widget ? widget->style() : QApplication::style();
		style->drawPrimitive(QStyle::PE_PanelItemViewItem, &opt, painter, widget);

		const PainterState state(painter);
		const QRect row = opt.rect;
		// Intersected rather than replaced, so the view's own clip still applies.
		painter->setClipRect(row, Qt::IntersectClip);
		const bool selected = opt.state.testFlag(QStyle::State_Selected);

		// The thumbnail sits at the left, centred vertically. The file icon stands in while the
		// picture loads, and when there is none.
		const QRect thumb_box(row.left() + kMargin,
				      row.top() + (row.height() - ClipListModel::kThumbHeight) / 2,
				      ClipListModel::kThumbWidth, ClipListModel::kThumbHeight);
		const QPixmap picture = index.data(Qt::DecorationRole).value<QPixmap>();
		if (picture.isNull()) {
			QRect icon_box(0, 0, kPlaceholderPx, kPlaceholderPx);
			icon_box.moveCenter(thumb_box.center());
			style->standardIcon(QStyle::SP_FileIcon).paint(painter, icon_box);
		} else {
			// Scaled in device pixels, so a HiDPI screen shows it sharp. The provider was asked
			// for twice the row's size for this reason.
			const qreal dpr = painter->device() ? painter->device()->devicePixelRatioF() : 1.0;
			QPixmap scaled = picture.scaled((QSizeF(thumb_box.size()) * dpr).toSize(), Qt::KeepAspectRatio,
							Qt::SmoothTransformation);
			scaled.setDevicePixelRatio(dpr);
			const QSizeF shown = QSizeF(scaled.size()) / dpr;
			const QPointF centre = QRectF(thumb_box).center();
			painter->drawPixmap(
				QPointF(centre.x() - shown.width() / 2.0, centre.y() - shown.height() / 2.0), scaled);
		}

		// The text starts past the thumbnail area and runs to the right margin.
		const int text_left = thumb_box.left() + thumb_box.width() + kGap;
		const int text_width = std::max(0, row.right() - kMargin - text_left + 1);
		const QFont font = opt.font;
		QFont small = font;
		// pointSizeF() is -1 for a font sized in pixels, and scaling that would give an invalid size.
		if (small.pointSizeF() > 0)
			small.setPointSizeF(font.pointSizeF() * kKindScale);
		const QFontMetrics metrics(font);
		const QFontMetrics small_metrics(small);
		const int block_height = metrics.height() * 2 + kLineGap;
		const int top = row.top() + (row.height() - block_height) / 2;
		const QRect name_line(text_left, top, text_width, metrics.height());
		const QRect details_line(text_left, top + metrics.height() + kLineGap, text_width, metrics.height());

		// The kind takes its room at the right of the first line; the file name is elided to what is left.
		const QString kind = kind_text(index.data(ClipListModel::KindRole).toInt());
		const int kind_room = kind.isEmpty() ? 0 : small_metrics.horizontalAdvance(kind) + kGap;
		const QString name = metrics.elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideMiddle,
							std::max(0, text_width - kind_room));

		// The details line: game, age and size. An unsorted clip has no game folder, so it says so.
		QString game = index.data(ClipListModel::GameRole).toString();
		if (game.isEmpty())
			game = module_text("Dock.Game.Unfiled");
		// U+00B7 MIDDLE DOT, as a code point so the source stays plain ASCII.
		const QString separator = QStringLiteral(" ") + QChar(0x00B7) + QStringLiteral(" ");
		const auto bytes = static_cast<std::uintmax_t>(index.data(ClipListModel::SizeRole).toULongLong());
		const auto modified = to_time_point(index.data(ClipListModel::ModifiedRole).toDateTime());
		const QString details = game + separator + age_text(modified, std::chrono::system_clock::now()) +
					separator + QString::fromStdString(format_size(bytes));
		const QString details_shown = metrics.elidedText(details, Qt::ElideRight, details_line.width());

		// On a selected row the text sits on the highlight, so it takes the highlighted-text colour.
		// Otherwise the details line uses the placeholder colour, which OBS themes dim.
		const QColor primary = opt.palette.color(selected ? QPalette::HighlightedText : QPalette::Text);
		const QColor secondary = selected ? primary : opt.palette.color(QPalette::PlaceholderText);

		painter->setFont(font);
		painter->setPen(primary);
		painter->drawText(name_line, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine, name);
		if (!kind.isEmpty()) {
			painter->setFont(small);
			painter->drawText(name_line, Qt::AlignRight | Qt::AlignVCenter | Qt::TextSingleLine, kind);
		}
		painter->setFont(font);
		painter->setPen(secondary);
		painter->drawText(details_line, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine, details_shown);
	} catch (const std::exception &e) {
		// Qt calls paint() from its event loop. The row is left partly drawn, which is better than a crash.
		obs_log(LOG_WARNING, "clip list: paint() failed: %s", e.what());
	} catch (...) {
		obs_log(LOG_WARNING, "clip list: paint() failed");
	}
}

} // namespace clip_atlas
