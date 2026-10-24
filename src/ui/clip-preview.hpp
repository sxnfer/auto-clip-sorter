// Clip Atlas — the floating window's preview of the current clip (the wide layout's right side).
//
//   ┌──────────────────────────────┐
//   │           picture            │  the whole frame, fitted, with rounded corners
//   └──────────────────────────────┘
//   Replay 21:46:58                    the title with the time, larger and bold
//   D:\Clips › Elden Ring › 2026-10    where the clip is, wrapped
//   Game   Elden Ring                  Dock.Preview.Game, .Kind, .Saved and .Size, muted, with their values
//   Type   Replay
//   Saved  23/10/2026 21:46
//   Size   84 MB
//   [Open] [Show in folder]            Dock.Action.Open, .Reveal, .Rename, .MoveToGame and .Recycle
//   [Rename…] [Move to game…]
//   [Move to Recycle Bin…]
// With no clip: Dock.Preview.Select, centred and muted.
//
// The picture is at most half the preview's height, so the facts and the buttons keep their room in a short window.
// It is asked for at kPicturePx from thumbnail_provider(); the list's smaller picture stands in until it arrives, and
// the kind's tile when there is none. Answers hop to the UI thread with run_on_ui(), guarded by a QPointer, and are
// kept only when their ticket is the newest request's. The last kCacheSize pictures are cached, keyed by path, size and
// modified time, so a replaced file is asked for again. The provider keeps one request per path, so the list asking
// for the same file drops the preview's request; a dropped request is asked again once, after kRetryMs, if its clip is
// still shown.
//
// UI thread only.
#pragma once

#include "lru-cache.hpp"
#include "thumbnails.hpp"

#include <QPixmap>
#include <QString>
#include <QWidget>

#include <cstdint>
#include <functional>
#include <string>

class QPushButton;

namespace clip_atlas {

class EmptyState;
class PreviewPicture;
class TextBlock;

struct PreviewInfo {
	std::string path_utf8;
	std::uintmax_t size = 0;
	std::int64_t modified_ms = 0;
	int kind = 0; // static_cast<int>(ClipKind)
	QString title;
	QString where;
	QString game;
	QString kind_text;
	QString saved;
	QString size_text;
	// The list's picture, shown until the larger one arrives. May be null.
	QPixmap fallback;
};

enum class PreviewAction {
	open,
	reveal,
	rename,
	move,
	recycle,
};

class ClipPreview : public QWidget {
public:
	static constexpr int kPicturePx = 640;
	static constexpr std::size_t kCacheSize = 8;
	static constexpr int kRetryMs = 400;

	using ActionCallback = std::function<void(PreviewAction action)>;

	explicit ClipPreview(QWidget *parent = nullptr);
	// Calls shutdown().
	~ClipPreview() override;

	ClipPreview(const ClipPreview &) = delete;
	ClipPreview &operator=(const ClipPreview &) = delete;

	void show_clip(const PreviewInfo &info);
	void show_nothing();
	// The list's picture for the clip shown, when it arrives after show_clip(). Ignored once the larger one is in.
	void set_fallback(const std::string &path_utf8, const QPixmap &picture);
	// The UTF-8 path shown, or "".
	const std::string &path() const { return info_.path_utf8; }
	void set_action_callback(ActionCallback callback);
	// Stops asking for pictures and ignores answers still on the way. Idempotent.
	void shutdown();

protected:
	void resizeEvent(QResizeEvent *event) override;
	// Redraws the buttons' icons for a new theme.
	void changeEvent(QEvent *event) override;

private:
	std::string cache_key(const PreviewInfo &info) const;
	void request_picture(int attempt);
	void on_picture(std::uint64_t ticket, int attempt, ThumbnailStatus status, const Thumbnail &thumb);
	void update_icons();
	void fit_picture();
	void run(PreviewAction action);

	PreviewInfo info_;
	bool has_clip_ = false;
	bool has_large_picture_ = false;
	bool stopped_ = false;
	std::uint64_t ticket_ = 0;
	LruCache<std::string, QPixmap> cache_{kCacheSize};
	ActionCallback action_callback_;

	QWidget *content_ = nullptr;
	EmptyState *nothing_ = nullptr;
	PreviewPicture *picture_ = nullptr;
	TextBlock *title_ = nullptr;
	TextBlock *where_ = nullptr;
	TextBlock *game_ = nullptr;
	TextBlock *kind_ = nullptr;
	TextBlock *saved_ = nullptr;
	TextBlock *size_ = nullptr;
	QPushButton *open_ = nullptr;
	QPushButton *reveal_ = nullptr;
	QPushButton *rename_ = nullptr;
	QPushButton *move_ = nullptr;
	QPushButton *recycle_ = nullptr;
};

} // namespace clip_atlas
