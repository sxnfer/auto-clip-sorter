// Clip Atlas — notices at the top of the dock for problems the user can fix.
//
// Each banner is a row: a standard icon (style()->standardIcon(
// QStyle::SP_MessageBoxWarning) for warnings, SP_MessageBoxInformation for
// information, 16 px), a word-wrapped QLabel with the text, an optional
// QPushButton with the action text, and for dismissible banners a flat "✕"
// QToolButton (tooltip Dock.Banner.Dismiss). The row is a QFrame with
// QFrame::StyledPanel, so the OBS theme draws it; no colours are set.
// Banners are stacked in BannerId order, whatever order they were shown in.
// The area is hidden when no banner is shown, so it takes no space.
//
// Dismissing hides the banner until show_banner() is called again with
// different text (the same text stays dismissed, so a repeated "update
// available" check doesn't bring back a banner the user closed).
//
// UI thread only.
#pragma once

#include <QString>
#include <QWidget>

#include <functional>
#include <map>

class QVBoxLayout;

namespace clip_atlas {

// Display order, top first.
enum class BannerId {
	no_clips_folder,        // no clips folder and no OBS recording folder
	clips_folder_missing,   // the clips folder doesn't exist (drive unplugged?)
	replay_buffer_disabled, // auto start is on but OBS's Replay Buffer is off
	update_available,       // M5
};

enum class BannerKind {
	warning,
	information,
};

class BannerArea : public QWidget {
public:
	using Action = std::function<void()>;

	explicit BannerArea(QWidget *parent = nullptr);

	// Shows banner `id`, or updates it if shown. An empty action_text gives no
	// button; the action runs on the UI thread when the button is clicked (the
	// banner stays; the action decides). `text` is shown as plain text
	// (Qt::PlainText), never as rich text.
	void show_banner(BannerId id, BannerKind kind, const QString &text, const QString &action_text = {},
			 Action action = {}, bool dismissible = false);
	// Hides banner `id`. Does nothing if it isn't shown.
	void hide_banner(BannerId id);
	// Shown and not dismissed.
	bool is_shown(BannerId id) const;

private:
	struct Banner;
	void rebuild();

	QVBoxLayout *layout_ = nullptr;
	std::map<BannerId, Banner *> banners_;
	std::map<BannerId, QString> dismissed_text_;
};

} // namespace clip_atlas
