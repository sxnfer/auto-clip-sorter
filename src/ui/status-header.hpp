// Clip Atlas — the top of the dock: which game is current, whether the Replay
// Buffer is running, and the auto-sort switch.
//
// Layout (docs/handover/M3-dock-spec.md):
//   ┌───────────────────────────────────────────┐
//   │ Counter-Strike 2                          │  row 1: game label (elided)
//   │ ● Replay Buffer on          [✓] Auto-sort │  row 2: pill + switch
//   └───────────────────────────────────────────┘
//   - Game label: the name passed to set_game(), or Dock.NoGame in the
//     palette's disabled text colour when it is empty. Elided at the right
//     with "…" when the dock is narrow; the full name is the tooltip.
//   - Replay Buffer pill: a QLabel with a filled dot (●, U+25CF) and text:
//       disabled_in_obs → Dock.Replay.Disabled, dot in the disabled text colour
//       inactive        → Dock.Replay.Off,      dot in the disabled text colour
//       active_by_us    → Dock.Replay.OnByUs,   dot in the Highlight colour
//       active_by_user  → Dock.Replay.On,       dot in the Highlight colour
//     Tooltip Dock.Replay.Tooltip. Colours come from palette() only, so every
//     OBS theme works; nothing is hard-coded.
//   - Auto-sort: a QCheckBox, Dock.AutoSort, tooltip Dock.AutoSort.Tooltip.
//     Clicking it calls the callback with the new state. set_auto_sort() never
//     calls it.
// Strings come from obs_module_text() (keys in data/locale/en-US.ini).
//
// UI thread only.
#pragma once

#include "replay-controller.hpp"

#include <QWidget>

#include <functional>

class QCheckBox;
class QLabel;

namespace clip_atlas {

class StatusHeader : public QWidget {
public:
	using AutoSortCallback = std::function<void(bool on)>;

	explicit StatusHeader(QWidget *parent = nullptr);

	// "" shows Dock.NoGame.
	void set_game(const QString &name);
	void set_replay_status(ReplayBufferStatus status);
	// Updates the checkbox without calling the callback.
	void set_auto_sort(bool on);
	void set_auto_sort_callback(AutoSortCallback callback);

protected:
	// Re-elides the game name.
	void resizeEvent(QResizeEvent *event) override;
	// Re-applies the palette colours when the OBS theme changes
	// (QEvent::PaletteChange, QEvent::StyleChange).
	void changeEvent(QEvent *event) override;

private:
	void update_game_label();
	void update_replay_label();

	QLabel *game_ = nullptr;
	QLabel *replay_ = nullptr;
	QCheckBox *auto_sort_ = nullptr;
	QString game_name_;
	ReplayBufferStatus replay_status_ = ReplayBufferStatus::inactive;
	AutoSortCallback auto_sort_callback_;
};

} // namespace clip_atlas
