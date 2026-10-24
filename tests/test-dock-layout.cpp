#include "dock-layout.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace clip_atlas;

TEST_CASE("choose_dock_layout: a side column, the bottom row and a floating window", "[dock-layout]")
{
	CHECK(choose_dock_layout(340, 700, DockLayout::column) == DockLayout::column);
	CHECK(choose_dock_layout(760, 250, DockLayout::column) == DockLayout::row);
	CHECK(choose_dock_layout(840, 540, DockLayout::column) == DockLayout::wide);
	// Narrow and short: there is no room for a strip either.
	CHECK(choose_dock_layout(320, 240, DockLayout::column) == DockLayout::column);
	// Wide but short: a strip, not a preview.
	CHECK(choose_dock_layout(1200, 280, DockLayout::column) == DockLayout::row);
}

TEST_CASE("choose_dock_layout enters each layout at its threshold", "[dock-layout]")
{
	using namespace dock_layout_limits;
	CHECK(choose_dock_layout(kRowMinWidth, kRowMaxHeight, DockLayout::column) == DockLayout::row);
	CHECK(choose_dock_layout(kRowMinWidth - 1, kRowMaxHeight, DockLayout::column) == DockLayout::column);
	CHECK(choose_dock_layout(kRowMinWidth, kRowMaxHeight + 1, DockLayout::column) == DockLayout::column);
	CHECK(choose_dock_layout(kWideMinWidth, kWideMinHeight, DockLayout::column) == DockLayout::wide);
	CHECK(choose_dock_layout(kWideMinWidth - 1, kWideMinHeight, DockLayout::column) == DockLayout::column);
	CHECK(choose_dock_layout(kWideMinWidth, kWideMinHeight - 1, DockLayout::column) == DockLayout::column);
}

TEST_CASE("choose_dock_layout keeps the current layout within the margin", "[dock-layout]")
{
	using namespace dock_layout_limits;
	// Row is kept a little taller and a little narrower than it is entered at.
	CHECK(choose_dock_layout(700, kRowMaxHeight + kMargin, DockLayout::row) == DockLayout::row);
	CHECK(choose_dock_layout(700, kRowMaxHeight + kMargin + 1, DockLayout::row) == DockLayout::column);
	CHECK(choose_dock_layout(kRowMinWidth - kMargin, 250, DockLayout::row) == DockLayout::row);
	CHECK(choose_dock_layout(kRowMinWidth - kMargin - 1, 250, DockLayout::row) == DockLayout::column);
	// Wide likewise.
	CHECK(choose_dock_layout(kWideMinWidth - kMargin, 500, DockLayout::wide) == DockLayout::wide);
	CHECK(choose_dock_layout(kWideMinWidth - kMargin - 1, 500, DockLayout::wide) == DockLayout::column);
	CHECK(choose_dock_layout(900, kWideMinHeight - kMargin, DockLayout::wide) == DockLayout::wide);
	// Too short for a preview, but too tall for the strip.
	CHECK(choose_dock_layout(900, kWideMinHeight - kMargin - 1, DockLayout::wide) == DockLayout::column);
	// Without the margin, the same sizes give column.
	CHECK(choose_dock_layout(kWideMinWidth - kMargin, 500, DockLayout::column) == DockLayout::column);
}

TEST_CASE("choose_dock_layout gives column for an empty or hidden dock", "[dock-layout]")
{
	CHECK(choose_dock_layout(0, 0, DockLayout::column) == DockLayout::column);
	CHECK(choose_dock_layout(0, 0, DockLayout::row) == DockLayout::column);
	CHECK(choose_dock_layout(-1, -1, DockLayout::wide) == DockLayout::column);
}
