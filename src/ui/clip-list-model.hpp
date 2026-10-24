// Clip Atlas — the dock's clip list model: the clips of a ClipIndex that match the filter, grouped by day, with their
// thumbnails. clip-list-view.hpp paints them.
//
// Rows (docs/handover/M6-dock-spec.md). Grouped, which the side column and the floating window use, each day group
// starts with a header row:
//   Today                                     8 clips · 1.6 GB      header: no item flags, so it can't be selected
//   [picture] Replay   New                                21:46     a clip
//             Elden Ring · 84 MB
//   - Groups: day_group_of(local_date_of(modified), today): Dock.Group.Today, .Yesterday, .Recent, then one header per
//     month, QLocale::system()'s standalone month name, with the year unless it is this year's. A header's detail is
//     Dock.Summary of Dock.Count.One / .Many and format_size() of its clips.
//   - Title (Qt::DisplayRole): clip_title(clip).
//   - Detail: game_label(game_folder) and format_size(), joined by " · ".
//   - When: the time of day (QLocale::system() short format) under Today and Yesterday, the short weekday and the time
//     under Previous 7 days, the short weekday and the day of the month under a month.
// Ungrouped, which the bottom row's strip of cards uses: no headers, and When reads alone: Dock.When.Today and
// .Yesterday with the time of day, the short weekday and the time for the previous five days, else the short date.
// The clip filed most recently in this session (set_fresh()) is Fresh, and its row flashes once.
//
// Thumbnails: data(row, Qt::DecorationRole) returns the cached QPixmap, or a
// null QPixmap while none is cached. A miss calls
// thumbnail_provider().request(path, kThumbRequestPx, ...) once per path (a
// set of pending paths), and the answer hops to the UI thread with
// run_on_ui(), guarded by a QPointer to the model:
//   ok      → QImage(bgra, w, h, w*4, Format_ARGB32_Premultiplied).copy() →
//             QPixmap, put in an LruCache<std::string, QPixmap> of
//             kThumbCacheSize, keyed by UTF-8 path; dataChanged for that row.
//   failed  → remembered (no retry until the clip changes); the placeholder stays.
//   dropped → forgotten, so the next paint asks again.
// The cache key is the path, so a renamed or replaced clip (upsert with a
// different size or modified time) drops its cached image.
//
// UI thread only.
#pragma once

#include "clip-display.hpp"
#include "clip-index.hpp"
#include "lru-cache.hpp"
#include "thumbnails.hpp"

#include <QAbstractListModel>
#include <QList>
#include <QPixmap>
#include <QString>

#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

class QVariantAnimation;

namespace clip_atlas {

// "Replay", "Replay (2)" or "Recording" for a name OBS made (parse_clip_title(); the kind's name stands in for an
// empty prefix), with the time when asked ("Replay 21:46:58"); any other name without its extension.
QString clip_title(const ClipEntry &clip, bool with_time = false);
// Dock.Kind.Replay, .Recording or .Screenshot.
QString kind_name(ClipKind kind);
// The game folder, or Dock.Game.Unfiled for a clip directly in the clips folder.
QString game_label(const std::string &game_folder);
// Dock.Count.One or Dock.Count.Many.
QString count_text(std::size_t count);
// Dock.Summary: "13 clips · 5.2 GB".
QString summary_text(std::size_t count, std::uintmax_t bytes);

class ClipListModel : public QAbstractListModel {
public:
	static constexpr int kThumbRequestPx = 256; // a list picture at 3x, or a card at 1x
	static constexpr std::size_t kThumbCacheSize = 240;

	enum Role {
		// QString: UTF-8 path as text (QString::fromStdString(path_to_utf8())).
		PathRole = Qt::UserRole + 1,
		// int: static_cast<int>(ClipKind).
		KindRole,
		// QString: game_folder.
		GameRole,
		// qulonglong: size in bytes.
		SizeRole,
		// QDateTime: modified, local time.
		ModifiedRole,
		// bool: a group header rather than a clip.
		HeaderRole,
		// QString: a clip's game and size, or a header's count and size.
		DetailRole,
		// QString: see the header.
		WhenRole,
		// bool: the clip filed most recently in this session.
		FreshRole,
		// qreal: 1 when the fresh clip's flash starts, down to 0; 0 for every other row.
		FlashRole,
	};

	explicit ClipListModel(QObject *parent = nullptr);
	// Calls shutdown().
	~ClipListModel() override;

	// A new scan: replaces every clip (beginResetModel/endResetModel). Drops
	// cached and pending thumbnails of paths that are gone.
	void reset_clips(std::vector<ClipEntry> clips);
	// A clip was filed, renamed or moved in: ClipIndex::upsert, then a reset
	// (keep the selection: the dock reselects by path).
	void upsert_clip(ClipEntry clip);
	// A clip was recycled or moved out. Removes its row and its thumbnail.
	void remove_clip(const std::filesystem::path &path);

	// Changes the filter (a reset). The clips are index().query(query).
	void set_query(ClipQuery query);
	const ClipQuery &query() const;
	// With or without day headers (a reset).
	void set_grouped(bool grouped);
	bool grouped() const { return grouped_; }

	// The whole index, unfiltered (for the game list and "Move to game…").
	const ClipIndex &index() const;
	// The index() above hides QAbstractItemModel's; this brings back
	// index(row, column, parent) for callers holding a ClipListModel.
	using QAbstractListModel::index;

	// The clip shown in `row`, or nullptr for a header or a row that doesn't exist. Valid until the next change.
	const ClipEntry *clip_at(int row) const;
	// The row showing `path`, or -1.
	int row_of(const std::filesystem::path &path) const;
	bool is_header(int row) const;
	// The clips listed, headers not counted.
	std::size_t clip_count() const;

	// Marks the clip at `path` fresh (Dock.New) and flashes its row. One clip at a time.
	void set_fresh(const std::filesystem::path &path);
	// The groups and the times depend on today's date: regroups (a reset) when it has changed since the rows were
	// built. The dock calls it once a minute. True if it regrouped.
	bool refresh_day();

	// Stops asking for thumbnails, stops the flash and ignores answers still
	// on the way. Idempotent.
	void shutdown();

	int rowCount(const QModelIndex &parent = QModelIndex()) const override;
	// Headers have no flags: they can't be selected or activated.
	Qt::ItemFlags flags(const QModelIndex &index) const override;
	// Qt::DisplayRole: a clip's title or a header's label. Qt::ToolTipRole: a clip's full path.
	// Qt::AccessibleTextRole: the title, detail and time. Qt::DecorationRole: see the header. The Role values above.
	QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;

private:
	struct Row {
		bool header = false;
		DayGroup group{DayGroupKind::today, {0, 0}};
		std::size_t clip = 0;     // clips_ index, for a clip row
		std::size_t count = 0;    // a header's clips
		std::uintmax_t bytes = 0; // and their total size
	};

	void rebuild_rows();
	QVariant header_data(const Row &row, int role) const;
	QVariant clip_data(const Row &row, int role) const;
	// Asks the provider for the picture of `path_utf8`, and marks the path pending under a new ticket.
	void request_thumbnail(const std::string &path_utf8) const;
	// The provider's answer, on the UI thread. It is ignored unless `ticket` is still the request pending
	// for the path: a forgotten or superseded request may describe a file that has changed since.
	void on_thumbnail(const std::string &path_utf8, std::uint64_t ticket, ThumbnailStatus status,
			  const Thumbnail &thumb);
	// Drops a path's picture, its pending request and its failed mark, because its file changed or went away.
	void forget_thumbnail(const std::string &path_utf8);
	void emit_row_changed(int row, const QList<int> &roles);

	ClipIndex index_;
	ClipQuery query_;
	bool grouped_ = true;
	// The query's clips, and the rows that show them. A clip row points into clips_.
	std::vector<ClipEntry> clips_;
	std::vector<Row> rows_;
	// The date the groups were worked out for.
	LocalDate today_{0, 0, 0};
	std::filesystem::path fresh_;
	int fresh_row_ = -1;
	qreal flash_level_ = 0.0;
	QVariantAnimation *flash_ = nullptr;
	// mutable: data() is const, and a cache miss starts a request.
	mutable LruCache<std::string, QPixmap> thumbnails_{kThumbCacheSize};
	// Path -> ticket of the request in flight. Tickets are never reused, so an answer for a request that was
	// forgotten or replaced can't be mistaken for the current one.
	mutable std::map<std::string, std::uint64_t> pending_;
	mutable std::set<std::string> failed_;
	mutable std::uint64_t next_ticket_ = 0;
	bool stopped_ = false;
};

} // namespace clip_atlas
