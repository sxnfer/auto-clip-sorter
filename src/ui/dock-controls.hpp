// Clip Atlas — the dock's painted controls: chips, the type filter, the switch, the game avatar and title, the Replay
// Buffer pill, notices, icon buttons, hairlines, text and the empty state.
//
// On Windows OBS keeps the native style under its style sheet, so standard widgets draw selection and check marks in
// Windows colours, and the style sheet's QWidget font rule overrides sizes set with setFont(). These controls paint
// themselves instead, from the palette (dock-style.hpp) and from the widget's own font, which is the theme's, scaled.
// None sets a style sheet. Each repaints when the palette changes and asks for a new size when the font changes.
// Buttons are QAbstractButtons, so they take focus, answer the keyboard and have accessible names like any button.
//
// UI thread only.
#pragma once

#include "dock-style.hpp"

#include <QAbstractButton>
#include <QFont>
#include <QFrame>
#include <QString>
#include <QWidget>

#include <functional>
#include <vector>

class QVariantAnimation;

namespace clip_atlas {

// A pill with a label and an optional count: one choice of a ChipBar, or the bar's "+N" button. Checked chips are
// filled with the accent.
class ChipButton : public QAbstractButton {
public:
	explicit ChipButton(QWidget *parent = nullptr);

	// A count below 0 shows none.
	void set_count(int count);
	int count() const { return count_; }

	QSize sizeHint() const override;
	QSize minimumSizeHint() const override;

protected:
	void paintEvent(QPaintEvent *event) override;
	void changeEvent(QEvent *event) override;

private:
	int count_ = -1;
};

// The game filter: one chip per choice, wrapped onto at most max_rows() rows. Chips that don't fit go into a "+N"
// chip's menu. The checked chip is always shown: when it would be in the menu, it takes the place of the last chip
// shown instead.
class ChipBar : public QWidget {
public:
	struct Entry {
		QString label;
		int count = -1; // below 0: no count
	};
	// Called with the index of the chip the user chose. Not called by set_entries() or set_current().
	using Chosen = std::function<void(int index)>;

	explicit ChipBar(QWidget *parent = nullptr);

	// Replaces the chips and checks `current` (clamped). The buttons are reused, so the focus stays on a chip whose
	// index still exists.
	void set_entries(const std::vector<Entry> &entries, int current);
	void set_current(int index);
	int current() const { return current_; }
	void set_chosen_callback(Chosen callback);
	// 1 for the bottom row's single line. At least 1.
	void set_max_rows(int rows);
	int max_rows() const { return max_rows_; }

	bool hasHeightForWidth() const override;
	int heightForWidth(int width) const override;
	QSize sizeHint() const override;
	QSize minimumSizeHint() const override;

protected:
	void resizeEvent(QResizeEvent *event) override;
	void changeEvent(QEvent *event) override;
	// Left and Right move the focus between the chips shown, so the bar is one Tab stop however many games it has.
	bool eventFilter(QObject *watched, QEvent *event) override;

private:
	struct Placement {
		std::vector<int> shown;   // chip indexes, in the order shown
		std::vector<QRect> rects; // rects[i] for shown[i]
		QRect more;               // the "+N" chip, or empty when every chip fits
		int hidden = 0;           // chips in the "+N" menu
		int rows = 1;
	};

	Placement place(int width) const;
	Placement place_in_order(int width, const std::vector<int> &order) const;
	int chip_height() const;
	int spacing() const;
	int more_width(int hidden) const;
	void relayout();
	void update_focus_chain();
	void choose(int index);
	void open_more_menu();

	std::vector<ChipButton *> chips_;
	ChipButton *more_ = nullptr;
	std::vector<int> shown_;  // chip indexes on screen, in order
	std::vector<int> hidden_; // chip indexes in the menu, in order
	int current_ = 0;
	int max_rows_ = 2;
	Chosen chosen_;
};

// One choice of a SegmentBar. Compact segments show only their icon, with the label as the tooltip.
class SegmentButton : public QAbstractButton {
public:
	SegmentButton(const QString &label, bool has_icon, DockIcon icon, QWidget *parent = nullptr);

	void set_compact(bool compact);
	bool compact() const { return compact_; }
	// The width the label needs, icon included.
	int full_width() const;
	// The width with the icon alone (or the label, for a segment without an icon).
	int compact_width() const;

	QSize sizeHint() const override;
	QSize minimumSizeHint() const override;

protected:
	void paintEvent(QPaintEvent *event) override;
	void changeEvent(QEvent *event) override;

private:
	bool has_icon_;
	DockIcon icon_;
	bool compact_ = false;
};

// The type filter: a track with one segment per choice. Segments share the width; when the labels don't fit, the
// segments with icons drop their labels.
class SegmentBar : public QWidget {
public:
	struct Segment {
		QString label;
		bool has_icon = false;
		DockIcon icon = DockIcon::replay;
	};
	using Chosen = std::function<void(int index)>;

	SegmentBar(const std::vector<Segment> &segments, QWidget *parent = nullptr);

	// Doesn't call the callback.
	void set_current(int index);
	int current() const { return current_; }
	void set_chosen_callback(Chosen callback);

	QSize sizeHint() const override;
	QSize minimumSizeHint() const override;

protected:
	void paintEvent(QPaintEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;
	void changeEvent(QEvent *event) override;
	// Left and Right choose the neighbouring segment, as in a native segmented control.
	bool eventFilter(QObject *watched, QEvent *event) override;

private:
	void relayout();
	void choose(int index);

	std::vector<SegmentButton *> segments_;
	int current_ = 0;
	Chosen chosen_;
};

// A switch with its label on the left. Clicking the label toggles it too. The knob slides when the state changes
// while the switch is visible.
class ToggleSwitch : public QAbstractButton {
public:
	explicit ToggleSwitch(const QString &text, QWidget *parent = nullptr);

	QSize sizeHint() const override;
	QSize minimumSizeHint() const override;

protected:
	void paintEvent(QPaintEvent *event) override;
	void changeEvent(QEvent *event) override;

private:
	QVariantAnimation *slide_ = nullptr;
	qreal knob_ = 0.0; // 0 off, 1 on; used only while slide_ runs
};

// The current game's initials on the accent, or a gamepad when there is no game. Square, sized from the font.
class GameAvatar : public QWidget {
public:
	explicit GameAvatar(QWidget *parent = nullptr);

	// "" shows the gamepad.
	void set_game(const QString &name);

	QSize sizeHint() const override;
	QSize minimumSizeHint() const override;

protected:
	void paintEvent(QPaintEvent *event) override;
	void changeEvent(QEvent *event) override;

private:
	QString initials_;
	bool has_game_ = false;
};

// A small muted line over a larger, bolder one: "Now playing" over the game. The title is elided to the width, and the
// full title is the tooltip when it doesn't fit.
class GameTitle : public QWidget {
public:
	explicit GameTitle(QWidget *parent = nullptr);

	// A dimmed title is drawn in the muted colour, for "No game detected".
	void set_texts(const QString &eyebrow, const QString &title, bool dimmed);

	QSize sizeHint() const override;
	QSize minimumSizeHint() const override;

protected:
	void paintEvent(QPaintEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;
	void changeEvent(QEvent *event) override;

private:
	QFont eyebrow_font() const;
	QFont title_font() const;
	void update_tooltip();

	QString eyebrow_;
	QString title_;
	bool dimmed_ = false;
};

// A rounded pill: a coloured dot, a status and an optional muted detail ("Replay Buffer on · started by Clip Atlas").
// When it is narrow the detail goes first, then the status is elided.
class StatusPill : public QWidget {
public:
	explicit StatusPill(QWidget *parent = nullptr);

	void set_status(const QColor &dot, const QString &text, const QString &detail);

	QSize sizeHint() const override;
	QSize minimumSizeHint() const override;

protected:
	void paintEvent(QPaintEvent *event) override;
	void changeEvent(QEvent *event) override;

private:
	QColor dot_;
	QString text_;
	QString detail_;
};

enum class NoticeTone {
	information,
	warning,
};

// A notice row: a rounded fill with a tone icon at the top left. The children (text, buttons) go in its layout, whose
// margins leave room for the icon.
class NoticeFrame : public QFrame {
public:
	explicit NoticeFrame(NoticeTone tone, QWidget *parent = nullptr);

	void set_tone(NoticeTone tone);
	// The left margin the frame's layout should keep for the icon.
	int icon_room() const;

protected:
	void paintEvent(QPaintEvent *event) override;
	void changeEvent(QEvent *event) override;

private:
	NoticeTone tone_;
};

// A 1 px line in the theme's line colour, across (Horizontal) or down (Vertical).
class HairLine : public QWidget {
public:
	explicit HairLine(Qt::Orientation orientation, QWidget *parent = nullptr);

	QSize sizeHint() const override;

protected:
	void paintEvent(QPaintEvent *event) override;

private:
	Qt::Orientation orientation_;
};

// A flat button: an icon that gets a rounded fill under the mouse, or a text link in the accent colour. Icon buttons
// use their tooltip as their accessible name.
class GhostButton : public QAbstractButton {
public:
	GhostButton(DockIcon icon, const QString &tooltip, QWidget *parent = nullptr);
	explicit GhostButton(const QString &text, QWidget *parent = nullptr);

	void set_icon(DockIcon icon);

	QSize sizeHint() const override;
	QSize minimumSizeHint() const override;

protected:
	void paintEvent(QPaintEvent *event) override;
	void changeEvent(QEvent *event) override;

private:
	bool has_icon_;
	DockIcon icon_ = DockIcon::more;
};

enum class TextTone {
	normal,
	muted,
	accent,
};

// Painted text in the widget's font, scaled. One line elided to the width, or wrapped (then it has a height for its
// width). An elided line has its full text as the tooltip.
class TextBlock : public QWidget {
public:
	explicit TextBlock(QWidget *parent = nullptr);
	TextBlock(const QString &text, TextTone tone, qreal scale = 1.0, QFont::Weight weight = QFont::Normal,
		  QWidget *parent = nullptr);

	void set_text(const QString &text);
	const QString &text() const { return text_; }
	void set_tone(TextTone tone);
	void set_scale(qreal scale, QFont::Weight weight = QFont::Normal);
	void set_wrap(bool wrap);
	void set_elide(Qt::TextElideMode mode);
	void set_alignment(Qt::Alignment alignment);

	bool hasHeightForWidth() const override;
	int heightForWidth(int width) const override;
	QSize sizeHint() const override;
	QSize minimumSizeHint() const override;

protected:
	void paintEvent(QPaintEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;
	void changeEvent(QEvent *event) override;

private:
	QFont text_font() const;
	void changed();
	void update_tooltip();

	QString text_;
	TextTone tone_ = TextTone::normal;
	qreal scale_ = 1.0;
	QFont::Weight weight_ = QFont::Normal;
	bool wrap_ = false;
	Qt::TextElideMode elide_ = Qt::ElideRight;
	Qt::Alignment alignment_ = Qt::AlignLeft | Qt::AlignVCenter;
};

// What the list area shows when there is nothing to list: a large muted icon, a title and a wrapped line, centred.
class EmptyState : public QWidget {
public:
	explicit EmptyState(QWidget *parent = nullptr);

	void set_texts(DockIcon icon, const QString &title, const QString &body);

	QSize sizeHint() const override;
	QSize minimumSizeHint() const override;

protected:
	void paintEvent(QPaintEvent *event) override;
	void changeEvent(QEvent *event) override;

private:
	DockIcon icon_ = DockIcon::replay;
	QString title_;
	QString body_;
};

// Paints a focus ring around `rect` when `widget` has the focus from the keyboard. A ring after every mouse click
// would be noise, so a click alone shows none.
void paint_focus_ring(QPainter &painter, const QWidget *widget, const QRectF &rect, qreal radius);

} // namespace clip_atlas
