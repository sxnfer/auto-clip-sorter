// Clip Atlas — the dock's clip list view and the delegate that paints it, as rows or as cards.
//
// Rows (the side column and the floating window), one per clip under its day header (clip-list-model.hpp):
//   Today                                              8 clips · 1.6 GB
//   ╭──────────────────────────────────────────────────────────────╮
//   │ ┌────────┐ Replay  (New)                                 21:46 │  a rounded fill on hover and selection
//   │ └[k]─────┘ Elden Ring · 84 MB                                  │  [k]: the kind's icon on the picture
//   ╰──────────────────────────────────────────────────────────────╯
//   - Picture: kPictureWidth x kPictureHeight with rounded corners, the thumbnail cropped to fill it. While it loads,
//     or when there is none, a tile in a tint of the base with the kind's icon.
//   - Title in the text colour, then Dock.New on the accent for the fresh clip; the detail line in the muted colour;
//     When at the right of the title line, muted.
//   - Hover: Open, Show in folder and More actions (Dock.Action.Open, .Reveal, .More as tooltips) take the place of
//     When. A click on one calls the action callback and doesn't activate the row.
//   - Selection: the selection fill (dock-style.hpp), so the text keeps its own colour on every theme. The fresh
//     clip's row flashes in the accent once (FlashRole).
// Cards (the bottom row): the strip runs left to right, a card per clip, as tall as the view:
//   ┌───────────────────────┐
//   │ Today 21:46      New  │  the picture, with When and Dock.New on it
//   │ [k]          ▶ 📁 ⋯  │  the kind's icon; the actions on hover
//   └───────────────────────┘
//   Replay
//   Elden Ring · 84 MB
//   A selected card has an accent outline.
//
// The view skips the headers when the arrow keys move the current row, scrolls the strip sideways with an ordinary
// mouse wheel, and sizes the cards to its height, keeping room for the scroll bar whether it shows or not, so the cards
// don't change size when it appears.
//
// UI thread only.
#pragma once

#include "dock-style.hpp"

#include <QListView>
#include <QPersistentModelIndex>
#include <QPoint>
#include <QPointer>
#include <QStyledItemDelegate>

#include <functional>

namespace clip_atlas {

class ClipListDelegate : public QStyledItemDelegate {
public:
	static constexpr int kPictureWidth = 80;
	static constexpr int kPictureHeight = 45;

	enum class Style {
		rows,
		cards,
	};
	enum class Action {
		open,
		reveal,
		more,
	};
	// `global` is where a menu for the action should open: the button's bottom left, in screen coordinates.
	using ActionCallback = std::function<void(const QModelIndex &index, Action action, const QPoint &global)>;

	// Follows the mouse over `view`'s viewport, which it needs for the hover actions.
	explicit ClipListDelegate(QAbstractItemView *view);

	void set_style(Style style);
	Style style() const { return style_; }
	// The cards' height, from the view's. Returns true if it changed.
	bool set_card_height(int height);
	int card_width() const;
	void set_action_callback(ActionCallback callback);
	// The mouse moved without a mouse event: the view scrolled under it.
	void update_hover(const QPoint &viewport_pos);

	void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
	// Rows: 0 wide, so QListView gives each row the viewport's width, and a header shorter than a clip. Cards: the
	// card's width and the view's height.
	QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;

	// The hover actions' tooltips; the path elsewhere.
	bool helpEvent(QHelpEvent *event, QAbstractItemView *view, const QStyleOptionViewItem &option,
		       const QModelIndex &index) override;

protected:
	// The clicks on the hover actions.
	bool editorEvent(QEvent *event, QAbstractItemModel *model, const QStyleOptionViewItem &option,
			 const QModelIndex &index) override;
	// The mouse over the viewport.
	bool eventFilter(QObject *watched, QEvent *event) override;

private:
	enum class Hit {
		none,
		open,
		reveal,
		more,
	};

	static DockIcon icon_of(Hit hit);
	void paint_header(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const;
	void paint_row(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const;
	void paint_card(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const;
	// The rects of the three actions in an item's rect.
	QRect action_rect(const QRect &item, Hit hit, const QFontMetrics &metrics) const;
	Hit hit_at(const QRect &item, const QPoint &pos, const QFontMetrics &metrics) const;
	QRect picture_rect(const QRect &item, const QFontMetrics &metrics) const;
	int text_block_height(const QFontMetrics &metrics) const;
	void set_hover(const QModelIndex &index, Hit hit);

	QPointer<QAbstractItemView> view_;
	Style style_ = Style::rows;
	int card_height_ = 200;
	ActionCallback action_callback_;
	// The item and the action under the mouse.
	QPersistentModelIndex hover_index_;
	Hit hover_hit_ = Hit::none;
	// The action pressed, until the release: it runs only if the release is on the same action.
	QPersistentModelIndex pressed_index_;
	Hit pressed_hit_ = Hit::none;
	// A double click on an action is followed by a release that must not run it a second time.
	bool swallow_release_ = false;
};

class ClipListView : public QListView {
public:
	explicit ClipListView(QWidget *parent = nullptr);

	ClipListDelegate *delegate() const { return delegate_; }
	// Cards left to right (the bottom row), or rows top to bottom.
	void set_strip(bool strip);
	bool strip() const { return strip_; }

protected:
	QModelIndex moveCursor(CursorAction action, Qt::KeyboardModifiers modifiers) override;
	void wheelEvent(QWheelEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;
	void scrollContentsBy(int dx, int dy) override;

private:
	void update_card_height();

	ClipListDelegate *delegate_ = nullptr;
	bool strip_ = false;
};

} // namespace clip_atlas
