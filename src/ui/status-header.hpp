// Clip Atlas — the top of the dock: which game is current, whether the Replay Buffer is running, the auto-sort switch
// and Save replay.
//
// Layouts (docs/handover/M6-dock-spec.md), the same widgets arranged for the dock's shape:
//   column   [avatar] Now playing                      Auto-sort (switch)
//                     Elden Ring
//            (● Replay Buffer on · started by Clip Atlas)       [⤓ Save replay]
//   rail     [avatar] Now playing                      the left of the bottom-row layout
//                     Elden Ring
//            (● Replay Buffer on · started by Clip Atlas)
//            Auto-sort (switch)                        [⤓ Save replay]
//   wide     [avatar] Now playing    (● Replay Buffer on · …)   Auto-sort (switch)   [⤓ Save replay]
//                     Elden Ring
//   - Avatar: initials_of() of the game on the accent; a gamepad when there is no game.
//   - Title: Dock.NowPlaying over the game, or Dock.WaitingForGame over Dock.NoGame, dimmed. The game is elided to the
//     room left, never widening the dock; the full name is the tooltip.
//   - Pill, by status:
//       active_by_us    success dot, Dock.Replay.On, detail Dock.Replay.ByUs
//       active_by_user  success dot, Dock.Replay.On
//       inactive        muted dot, Dock.Replay.Off, detail Dock.Replay.WithGame while auto start is on
//       disabled_in_obs warning dot, Dock.Replay.Disabled
//     Tooltip Dock.Replay.Tooltip. The success and warning colours are the theme's (theme_accents()).
//   - Auto-sort: a ToggleSwitch, Dock.AutoSort, tooltip Dock.AutoSort.Tooltip. Clicking it calls the auto-sort
//     callback with the new state; set_auto_sort() never calls it.
//   - Save replay: a QPushButton with the save icon and Dock.SaveReplay (the icon alone, with the text as its tooltip
//     and accessible name, when its row is too narrow for both), tooltip Dock.SaveReplay.Tooltip. Enabled while the
//     buffer runs. Clicking it calls the save callback.
// Strings come from obs_module_text() (keys in data/locale/en-US.ini).
//
// UI thread only.
#pragma once

#include "replay-controller.hpp"

#include <QWidget>

#include <functional>

class QPushButton;

namespace clip_atlas {

class GameAvatar;
class GameTitle;
class StatusPill;
class ToggleSwitch;

class StatusHeader : public QWidget {
public:
	enum class Layout {
		column,
		rail,
		wide,
	};
	using AutoSortCallback = std::function<void(bool on)>;
	using SaveCallback = std::function<void()>;

	explicit StatusHeader(QWidget *parent = nullptr);

	void set_layout(Layout layout);
	Layout layout_kind() const { return layout_; }

	// "" shows Dock.NoGame.
	void set_game(const QString &name);
	void set_replay_status(ReplayBufferStatus status);
	// Whether Clip Atlas starts the buffer with the next game, which the stopped pill mentions.
	void set_replay_auto_start(bool on);
	// Updates the switch without calling the callback.
	void set_auto_sort(bool on);
	void set_auto_sort_callback(AutoSortCallback callback);
	void set_save_callback(SaveCallback callback);

protected:
	// Fits the Save replay button to its row.
	void resizeEvent(QResizeEvent *event) override;
	// Redraws the icon and the pill when the OBS theme changes (QEvent::PaletteChange, QEvent::StyleChange).
	void changeEvent(QEvent *event) override;

private:
	void rebuild_layout();
	void update_title();
	void update_pill();
	void update_save_button();

	Layout layout_ = Layout::column;
	GameAvatar *avatar_ = nullptr;
	GameTitle *title_ = nullptr;
	StatusPill *pill_ = nullptr;
	ToggleSwitch *auto_sort_ = nullptr;
	QPushButton *save_ = nullptr;
	QString game_name_;
	ReplayBufferStatus replay_status_ = ReplayBufferStatus::inactive;
	bool replay_auto_start_ = false;
	AutoSortCallback auto_sort_callback_;
	SaveCallback save_callback_;
};

} // namespace clip_atlas
