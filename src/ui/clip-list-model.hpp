// Clip Atlas — the dock's clip list: a Qt model over ClipIndex, and the
// delegate that draws each row.
//
// Row layout (docs/handover/M3-dock-spec.md), kRowHeight px tall:
//   ┌──────────┐ Replay 2026-10-21 18-04-12.mkv          [Replay]
//   │ thumbnail│ Counter-Strike 2 · 2 min ago · 84 MB
//   └──────────┘
//   - Thumbnail: kThumbWidth x kThumbHeight, the image scaled to fit (keep
//     aspect ratio, centred, smooth). While it loads, or when there is none, a
//     placeholder: style()->standardIcon(QStyle::SP_FileIcon) centred.
//   - Line 1: file name, elided in the middle (Qt::ElideMiddle) to fit.
//   - Line 2, in the palette's placeholder/disabled text colour: game label,
//     age, size, joined by " · ". Game label: game_folder, or
//     Dock.Game.Unfiled for "". Age: describe_age() → Dock.Age.JustNow /
//     Dock.Age.Minutes / .Hours / .Days (each with %1 for the count, replaced
//     with QString::arg) or format_local_date(). Size: format_size().
//   - Right of line 1: the kind as small text (Dock.Kind.Replay /
//     .Recording / .Screenshot).
//   - Selection and hover come from the style (QStyle::PE_PanelItemViewItem
//     through QStyledItemDelegate's option), so every OBS theme works. No
//     colour is hard-coded.
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

#include "clip-index.hpp"
#include "lru-cache.hpp"
#include "thumbnails.hpp"

#include <QAbstractListModel>
#include <QPixmap>
#include <QStyledItemDelegate>

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace clip_atlas {

class ClipListModel : public QAbstractListModel {
public:
	static constexpr int kRowHeight = 64;
	static constexpr int kThumbWidth = 96;
	static constexpr int kThumbHeight = 54;
	static constexpr int kThumbRequestPx = 192; // 2x for high-DPI screens
	static constexpr std::size_t kThumbCacheSize = 300;

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
	};

	explicit ClipListModel(QObject *parent = nullptr);
	// Calls shutdown().
	~ClipListModel() override;

	// A new scan: replaces every clip (beginResetModel/endResetModel). Drops
	// cached and pending thumbnails of paths that are gone.
	void reset_clips(std::vector<ClipEntry> clips);
	// A clip was filed, renamed or moved in: ClipIndex::upsert, then the rows
	// change with the matching begin/end insert/remove/move calls, or a reset
	// when that is simpler (keep the selection: the dock reselects by path).
	void upsert_clip(ClipEntry clip);
	// A clip was recycled or moved out. Removes its row and its thumbnail.
	void remove_clip(const std::filesystem::path &path);

	// Changes the filter (a reset). The rows are index().query(query).
	void set_query(ClipQuery query);
	const ClipQuery &query() const;

	// The whole index, unfiltered (for the game list and "Move to game…").
	const ClipIndex &index() const;
	// The index() above hides QAbstractItemModel's; this brings back
	// index(row, column, parent) for callers holding a ClipListModel.
	using QAbstractListModel::index;

	// The clip shown in `row`, or nullptr. Valid until the next change.
	const ClipEntry *clip_at(int row) const;
	// The row showing `path`, or -1.
	int row_of(const std::filesystem::path &path) const;

	// Ages change as time passes: dataChanged for every row (the dock calls
	// it once a minute). No reset, so the selection and scroll stay.
	void refresh_ages();

	// Stops asking for thumbnails and ignores answers still on the way.
	// Idempotent.
	void shutdown();

	int rowCount(const QModelIndex &parent = QModelIndex()) const override;
	// Qt::DisplayRole: file name. Qt::ToolTipRole: the full path.
	// Qt::DecorationRole: see the header. The Role values above.
	QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;

private:
	void rebuild_rows();
	// Asks the provider for the picture of `path_utf8`, and marks the path pending under a new ticket.
	void request_thumbnail(const std::string &path_utf8) const;
	// The provider's answer, on the UI thread. It is ignored unless `ticket` is still the request pending
	// for the path: a forgotten or superseded request may describe a file that has changed since.
	void on_thumbnail(const std::string &path_utf8, std::uint64_t ticket, ThumbnailStatus status,
			  const Thumbnail &thumb);
	// Drops a path's picture, its pending request and its failed mark, because its file changed or went away.
	void forget_thumbnail(const std::string &path_utf8);

	ClipIndex index_;
	ClipQuery query_;
	std::vector<ClipEntry> rows_;
	// mutable: data() is const, and a cache miss starts a request.
	mutable LruCache<std::string, QPixmap> thumbnails_{kThumbCacheSize};
	// Path -> ticket of the request in flight. Tickets are never reused, so an answer for a request that was
	// forgotten or replaced can't be mistaken for the current one.
	mutable std::map<std::string, std::uint64_t> pending_;
	mutable std::set<std::string> failed_;
	mutable std::uint64_t next_ticket_ = 0;
	bool stopped_ = false;
};

// Paints a row as the header describes. sizeHint: kRowHeight high and 0 wide,
// so QListView sizes each row to its viewport (the option's width is the whole
// view's and would push the right edge under a vertical scrollbar).
class ClipListDelegate : public QStyledItemDelegate {
public:
	explicit ClipListDelegate(QObject *parent = nullptr);

	void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
	QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;
};

} // namespace clip_atlas
