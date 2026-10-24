// Clip Atlas — the note that floats over the bottom of the clip list after a clip is filed.
//
//   ╭───────────────────────────────────────────────╮
//   │ (✓)  Filed in Elden Ring › 2026-10      Show  │   title, bold, wrapped
//   │      Replay 21:46:58 · 84 MB                  │   detail, muted, wrapped
//   ╰───────────────────────────────────────────────╯
//   - The badge is the theme's success colour (a check) or warning colour (a warning sign), from theme_accents().
//   - Show (Dock.Toast.Show, a GhostButton) runs the action and closes the toast.
//   - It fades in, stays kShowMs and fades out. The mouse over it pauses the countdown, which starts again, shorter,
//     when the mouse leaves. A new toast replaces the one shown.
// The toast is a child of the dock but in no layout: the dock tells it the area it may cover (set_area()), and it sits
// centred at the bottom of that area, no wider than kMaxWidth. It paints its own rounded card and soft shadow.
//
// UI thread only.
#pragma once

#include <QRect>
#include <QString>
#include <QWidget>

#include <functional>

class QGraphicsOpacityEffect;
class QTimer;
class QVariantAnimation;

namespace clip_atlas {

class GhostButton;
class TextBlock;

enum class ToastTone {
	success,
	warning,
};

class DockToast : public QWidget {
public:
	static constexpr int kShowMs = 5000;
	static constexpr int kResumeMs = 2500;
	static constexpr int kMaxWidth = 440;

	using Action = std::function<void()>;

	explicit DockToast(QWidget *parent);

	// An empty `detail` leaves out the second line. An empty action leaves out Show.
	void show_toast(ToastTone tone, const QString &title, const QString &detail, Action action);
	// Fades out now. Does nothing while hidden.
	void dismiss();
	// The area the toast may cover, in the parent's coordinates.
	void set_area(const QRect &area);
	// Hides the toast at once and stops its timers. Idempotent.
	void shutdown();

protected:
	void paintEvent(QPaintEvent *event) override;
	void enterEvent(QEnterEvent *event) override;
	void leaveEvent(QEvent *event) override;
	void changeEvent(QEvent *event) override;

private:
	void reposition();
	void fade_to(qreal opacity);
	void run_action();

	TextBlock *title_ = nullptr;
	TextBlock *detail_ = nullptr;
	GhostButton *show_ = nullptr;
	QTimer *countdown_ = nullptr;
	QGraphicsOpacityEffect *opacity_ = nullptr;
	QVariantAnimation *fade_ = nullptr;
	QRect area_;
	ToastTone tone_ = ToastTone::success;
	Action action_;
	bool stopped_ = false;
};

} // namespace clip_atlas
