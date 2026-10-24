// Clip Atlas — which arrangement the dock uses for the room it has.
//
// OBS docks end up in a tall side column, in the short bottom row next to the Audio Mixer, or floating. The dock keeps
// one set of widgets and arranges them three ways:
//   column  header, filters, list and footer stacked: a side column, and anything that fits nothing else
//   row     short and wide: the header becomes a rail on the left and the clips a sideways strip of cards
//   wide    large: the list on the left and a preview of the selected clip on the right
// Row wins over wide, because a short dock has no height for a preview. Each threshold has a margin: a dock already
// in a layout keeps it until it is kMargin past the threshold it entered by, so a resize near the line doesn't flip
// the layout back and forth.
//
// Pure C++17. Sizes are Qt logical pixels.
#pragma once

namespace clip_atlas {

enum class DockLayout {
	column,
	row,
	wide,
};

namespace dock_layout_limits {
// row: no taller than this and at least this wide.
constexpr int kRowMaxHeight = 290;
constexpr int kRowMinWidth = 480;
// wide: at least this wide and this tall.
constexpr int kWideMinWidth = 760;
constexpr int kWideMinHeight = 420;
// How far past a threshold the current layout is kept.
constexpr int kMargin = 30;
} // namespace dock_layout_limits

// The layout for a dock `width` x `height` that currently uses `current`.
DockLayout choose_dock_layout(int width, int height, DockLayout current);

} // namespace clip_atlas
