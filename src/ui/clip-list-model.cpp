// Clip Atlas — the dock's clip list model. clip-list-model.hpp says what each row shows; this file groups the clips by
// day and keeps the thumbnail cache and the pending requests in step with them.
#include "clip-list-model.hpp"

#include "clip-layout.hpp"
#include "ui-thread.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <QChar>
#include <QDate>
#include <QDateTime>
#include <QImage>
#include <QLocale>
#include <QPointer>
#include <QRegularExpression>
#include <QTime>
#include <QVariantAnimation>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <exception>
#include <memory>
#include <optional>
#include <utility>

namespace clip_atlas {

namespace {

// The fresh clip's flash: from the accent to nothing, long enough to catch the eye and short enough not to nag.
constexpr int kFlashMs = 1400;

QString module_text(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

// The core's strings are UTF-8 std::strings. The explicit length keeps an embedded NUL from cutting the text short.
QString utf8(const std::string &text)
{
	return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

// U+00B7 MIDDLE DOT between spaces, as a code point so the source stays plain ASCII.
QString separator()
{
	return QStringLiteral(" ") + QChar(0x00B7) + QStringLiteral(" ");
}

// system_clock counts from the Unix epoch: C++20 requires it, and MSVC and libstdc++ already do in C++17. Milliseconds
// is the precision QDateTime keeps, so a clip's modified time survives the trip to the delegate.
QDateTime to_date_time(std::chrono::system_clock::time_point t)
{
	const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count();
	return QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(ms));
}

// The locale's short time without the seconds: "21:46" or "9:46 PM". Some locales' short format has seconds, and a
// list of times to the second is noise.
QString time_of_day(const QDateTime &t)
{
	static const QRegularExpression seconds(QStringLiteral("[:.]?s{1,2}"));
	QString format = QLocale::system().timeFormat(QLocale::ShortFormat);
	format.remove(seconds);
	return QLocale::system().toString(t.time(), format.trimmed());
}

QString short_weekday(const QDateTime &t)
{
	return QLocale::system().dayName(t.date().dayOfWeek(), QLocale::ShortFormat);
}

// Under a header, which already says the day or the month.
QString when_in_group(const QDateTime &t, DayGroupKind group)
{
	switch (group) {
	case DayGroupKind::today:
	case DayGroupKind::yesterday:
		return time_of_day(t);
	case DayGroupKind::recent:
		return short_weekday(t) + QLatin1Char(' ') + time_of_day(t);
	case DayGroupKind::month:
		return short_weekday(t) + QLatin1Char(' ') + QString::number(t.date().day());
	}
	return time_of_day(t);
}

// On a card, which has no header.
QString when_alone(const QDateTime &t, DayGroupKind group)
{
	switch (group) {
	case DayGroupKind::today:
		return module_text("Dock.When.Today").arg(time_of_day(t));
	case DayGroupKind::yesterday:
		return module_text("Dock.When.Yesterday").arg(time_of_day(t));
	case DayGroupKind::recent:
		return short_weekday(t) + QLatin1Char(' ') + time_of_day(t);
	case DayGroupKind::month:
		break;
	}
	return QLocale::system().toString(t.date(), QLocale::ShortFormat);
}

QString group_label(const DayGroup &group, const LocalDate &today)
{
	switch (group.kind) {
	case DayGroupKind::today:
		return module_text("Dock.Group.Today");
	case DayGroupKind::yesterday:
		return module_text("Dock.Group.Yesterday");
	case DayGroupKind::recent:
		return module_text("Dock.Group.Recent");
	case DayGroupKind::month:
		break;
	}
	// The standalone name ("Oktober", not the genitive some languages use inside a date). This year's months need no
	// year.
	const QString month = QLocale::system().standaloneMonthName(group.month.month, QLocale::LongFormat);
	if (group.month.year == today.year)
		return month;
	return month + QLatin1Char(' ') + QString::number(group.month.year);
}

} // namespace

QString clip_title(const ClipEntry &clip, bool with_time)
{
	const ClipTitle parsed = parse_clip_title(clip.file_name);
	if (!parsed.stamped)
		return utf8(parsed.stem);
	QString title = parsed.prefix.empty() ? kind_name(clip.kind) : utf8(parsed.prefix);
	if (with_time)
		title += QLatin1Char(' ') + utf8(parsed.time);
	return title + utf8(parsed.suffix);
}

QString kind_name(ClipKind kind)
{
	switch (kind) {
	case ClipKind::replay:
		return module_text("Dock.Kind.Replay");
	case ClipKind::recording:
		return module_text("Dock.Kind.Recording");
	case ClipKind::screenshot:
		return module_text("Dock.Kind.Screenshot");
	}
	return QString();
}

QString game_label(const std::string &game_folder)
{
	return game_folder.empty() ? module_text("Dock.Game.Unfiled") : utf8(game_folder);
}

QString count_text(std::size_t count)
{
	if (count == 1)
		return module_text("Dock.Count.One");
	return module_text("Dock.Count.Many").arg(static_cast<qulonglong>(count));
}

QString summary_text(std::size_t count, std::uintmax_t bytes)
{
	return module_text("Dock.Summary").arg(count_text(count), utf8(format_size(bytes)));
}

ClipListModel::ClipListModel(QObject *parent) : QAbstractListModel(parent)
{
	flash_ = new QVariantAnimation(this);
	flash_->setDuration(kFlashMs);
	flash_->setStartValue(1.0);
	flash_->setEndValue(0.0);
	flash_->setEasingCurve(QEasingCurve::InQuad);
	QObject::connect(flash_, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
		flash_level_ = value.toReal();
		emit_row_changed(fresh_row_, {FlashRole});
	});
	today_ = local_date_of(std::chrono::system_clock::now());
}

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
	asked_px_.clear();
	index_.reset(std::move(clips));
	rebuild_rows();
	endResetModel();
}

void ClipListModel::upsert_clip(ClipEntry clip)
{
	// A reset rather than row-by-row inserts: one change can move a row, add a header, or push another
	// clip out past the cap, and the dock reselects by path anyway.
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

void ClipListModel::set_grouped(bool grouped)
{
	if (grouped == grouped_)
		return;
	beginResetModel();
	grouped_ = grouped;
	rebuild_rows();
	endResetModel();
}

const ClipIndex &ClipListModel::index() const
{
	return index_;
}

const ClipEntry *ClipListModel::clip_at(int row) const
{
	if (row < 0 || row >= static_cast<int>(rows_.size()))
		return nullptr;
	const Row &shown = rows_[static_cast<std::size_t>(row)];
	return shown.header ? nullptr : &clips_[shown.clip];
}

int ClipListModel::row_of(const std::filesystem::path &path) const
{
	for (std::size_t i = 0; i < rows_.size(); ++i) {
		if (!rows_[i].header && clips_[rows_[i].clip].path == path)
			return static_cast<int>(i);
	}
	return -1;
}

bool ClipListModel::is_header(int row) const
{
	return row >= 0 && row < static_cast<int>(rows_.size()) && rows_[static_cast<std::size_t>(row)].header;
}

std::size_t ClipListModel::clip_count() const
{
	return clips_.size();
}

void ClipListModel::set_fresh(const std::filesystem::path &path)
{
	if (stopped_)
		return;
	const int old_row = fresh_row_;
	fresh_ = path;
	fresh_row_ = row_of(path);
	if (old_row >= 0 && old_row != fresh_row_)
		emit_row_changed(old_row, {FreshRole, FlashRole});
	flash_->stop();
	flash_level_ = 1.0;
	emit_row_changed(fresh_row_, {FreshRole, FlashRole});
	flash_->start();
}

void ClipListModel::set_picture_px(int px)
{
	picture_px_ = std::max(16, px);
}

bool ClipListModel::refresh_day()
{
	if (local_date_of(std::chrono::system_clock::now()) == today_)
		return false;
	beginResetModel();
	rebuild_rows();
	endResetModel();
	return true;
}

void ClipListModel::shutdown()
{
	// Answers still on the way find no pending request and are ignored; stopped_ turns away new ones.
	stopped_ = true;
	pending_.clear();
	if (flash_)
		flash_->stop();
	flash_level_ = 0.0;
}

int ClipListModel::rowCount(const QModelIndex &parent) const
{
	// A flat list: only the root has rows.
	return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

Qt::ItemFlags ClipListModel::flags(const QModelIndex &index) const
{
	if (!index.isValid() || is_header(index.row()))
		return Qt::NoItemFlags;
	return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
}

QVariant ClipListModel::data(const QModelIndex &index, int role) const
{
	try {
		if (!index.isValid() || index.row() >= static_cast<int>(rows_.size()))
			return QVariant();
		const Row &row = rows_[static_cast<std::size_t>(index.row())];
		return row.header ? header_data(row, role) : clip_data(row, role);
	} catch (const std::exception &e) {
		// Qt calls data() from its event loop, so nothing may leave it. The cell falls back to "no data".
		obs_log(LOG_WARNING, "clip list: data() failed: %s", e.what());
		return QVariant();
	} catch (...) {
		obs_log(LOG_WARNING, "clip list: data() failed");
		return QVariant();
	}
}

QVariant ClipListModel::header_data(const Row &row, int role) const
{
	switch (role) {
	case Qt::DisplayRole:
		return group_label(row.group, today_);
	case Qt::AccessibleTextRole:
		return group_label(row.group, today_) + QStringLiteral(", ") + summary_text(row.count, row.bytes);
	case HeaderRole:
		return true;
	case DetailRole:
		return summary_text(row.count, row.bytes);
	default:
		return QVariant();
	}
}

QVariant ClipListModel::clip_data(const Row &row, int role) const
{
	const ClipEntry &clip = clips_[row.clip];
	switch (role) {
	case Qt::DisplayRole:
		return clip_title(clip);
	case Qt::ToolTipRole:
		return utf8(path_to_utf8(clip.path));
	case Qt::AccessibleTextRole: {
		// The standalone time: a screen reader says the row without its header.
		const QString when = when_alone(to_date_time(clip.modified), row.group.kind);
		return clip_title(clip) + QStringLiteral(", ") + game_label(clip.game_folder) + QStringLiteral(", ") +
		       utf8(format_size(clip.size)) + QStringLiteral(", ") + when;
	}
	case Qt::DecorationRole: {
		const std::string key = path_to_utf8(clip.path);
		if (QPixmap *picture = thumbnails_.get(key)) {
			// A picture made for the list is too small for a card. It stands in while a larger one is
			// asked for, once.
			const int longest = std::max(picture->width(), picture->height());
			const auto asked = asked_px_.find(key);
			const bool asked_enough = asked != asked_px_.end() && asked->second >= picture_px_;
			if (longest < picture_px_ * 9 / 10 && !asked_enough && !stopped_ && pending_.count(key) == 0)
				request_thumbnail(key);
			return *picture;
		}
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
	case HeaderRole:
		return false;
	case DetailRole:
		return game_label(clip.game_folder) + separator() + utf8(format_size(clip.size));
	case WhenRole: {
		const QDateTime modified = to_date_time(clip.modified);
		return grouped_ ? when_in_group(modified, row.group.kind) : when_alone(modified, row.group.kind);
	}
	case FreshRole:
		return !fresh_.empty() && clip.path == fresh_;
	case FlashRole:
		return !fresh_.empty() && clip.path == fresh_ ? flash_level_ : 0.0;
	default:
		return QVariant();
	}
}

void ClipListModel::rebuild_rows()
{
	clips_ = index_.query(query_);
	today_ = local_date_of(std::chrono::system_clock::now());
	rows_.clear();
	rows_.reserve(clips_.size() + 8);
	// The clips are newest first, so each group's clips are together and one header starts each group.
	std::optional<std::size_t> header;
	for (std::size_t i = 0; i < clips_.size(); ++i) {
		const DayGroup group = day_group_of(local_date_of(clips_[i].modified), today_);
		if (grouped_ && (!header || rows_[*header].group != group)) {
			header = rows_.size();
			Row start;
			start.header = true;
			start.group = group;
			rows_.push_back(start);
		}
		if (header) {
			rows_[*header].count += 1;
			rows_[*header].bytes += clips_[i].size;
		}
		Row row;
		row.group = group;
		row.clip = i;
		rows_.push_back(row);
	}
	fresh_row_ = fresh_.empty() ? -1 : row_of(fresh_);
}

void ClipListModel::request_thumbnail(const std::string &path_utf8) const
{
	const std::uint64_t ticket = ++next_ticket_;
	pending_[path_utf8] = ticket;
	int &asked = asked_px_[path_utf8];
	asked = std::max(asked, picture_px_);
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
	thumbnail_provider().request(path_utf8, picture_px_, std::move(answered));
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
		// The next paint asks again, a larger picture included.
		asked_px_.erase(path_utf8);
		return;
	}

	// The row may have been filtered out or removed since the request. Then the picture waits in the cache for it.
	emit_row_changed(row_of(path_from_utf8(path_utf8)), {Qt::DecorationRole});
}

void ClipListModel::forget_thumbnail(const std::string &path_utf8)
{
	thumbnails_.erase(path_utf8);
	pending_.erase(path_utf8);
	failed_.erase(path_utf8);
	asked_px_.erase(path_utf8);
}

void ClipListModel::emit_row_changed(int row, const QList<int> &roles)
{
	if (row < 0 || row >= static_cast<int>(rows_.size()))
		return;
	const QModelIndex changed = QAbstractListModel::index(row, 0);
	emit dataChanged(changed, changed, roles);
}

} // namespace clip_atlas
