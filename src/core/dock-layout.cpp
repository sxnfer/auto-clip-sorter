// Clip Atlas — the dock's layout for its size. dock-layout.hpp gives the thresholds and why they have a margin.
#include "dock-layout.hpp"

namespace clip_atlas {

DockLayout choose_dock_layout(int width, int height, DockLayout current)
{
	using namespace dock_layout_limits;
	// The current layout gets the margin on each of its own limits.
	const int row_margin = current == DockLayout::row ? kMargin : 0;
	if (height <= kRowMaxHeight + row_margin && width >= kRowMinWidth - row_margin)
		return DockLayout::row;
	const int wide_margin = current == DockLayout::wide ? kMargin : 0;
	if (width >= kWideMinWidth - wide_margin && height >= kWideMinHeight - wide_margin)
		return DockLayout::wide;
	return DockLayout::column;
}

} // namespace clip_atlas
