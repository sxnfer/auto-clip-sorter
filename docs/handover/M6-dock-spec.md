# M6 dock spec: Native Plus (WP6.0, orchestrator)

The M3 dock, redrawn and rearranged for the room it gets. The data, the actions, the safety rules and the threads
are M3's (see `M3-dock-spec.md`); this file says what changes. The design is concept A of
`docs/design/dock-explorations.md`. Where this file and a header disagree, the header wins.

## Three layouts

OBS docks end up in a tall side column, in the short bottom row next to the Audio Mixer, or floating.
`core/dock-layout`'s `choose_dock_layout(width, height, current)` picks one of three arrangements of the same
widgets, and `ClipDock` re-lays them out when the answer changes (queued from `resizeEvent`, never inside it).

```
column (default)                               row (short and wide)
┌─ Clip Atlas ─────────────────────────┐      ┌─ Clip Atlas ───────────────────────────────────────────┐
│ [ER] Now playing          Auto-sort ◉│      │ [ER] Now playing   │ (All 13)(Elden Ring 4)…  [🔍 Search] │
│      Elden Ring                      │      │      Elden Ring    │ ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐  │
│ (● Replay Buffer on · started…) [⤓ Save replay] │ (● Buffer on…)  │ │ card │ │ card │ │ card │ │ card │  │
│ ⚠ banner                             │      │ Auto-sort ◉ [⤓ Save]│ └──────┘ └──────┘ └──────┘ └──────┘  │
│ [🔍 Search clips                  ✕] │      │ ✓ Filed in …       │  title / game · size                 │
│ (All 13)(Elden Ring 4)(CS2 2) …      │      │ 13 clips · 5.2 GB 📁⟳⚙│                                      │
│ [ All | Replays | Recordings | Shots ]│      └────────────────────────────────────────────────────────┘
├──────────────────────────────────────┤
│ Today                8 clips · 1.6 GB│      wide (large, usually floating)
│ [pic] Replay 21:46:58  New      21:46│      ┌─ Clip Atlas ───────────────────────────────────────────┐
│       Elden Ring · 84 MB    (hover ▶📁⋯)    │ [ER] Now playing · Elden Ring  (● Buffer on)  Auto-sort ◉ [⤓ Save] │
│ Yesterday                            │      ├──────────────────────┬─────────────────────────────────┤
│ …                                    │      │ search, chips, kinds │  ┌───────── picture ──────────┐ │
│ ┌ ✓ Filed in Elden Ring › 2026-10 [Show] ┐ │ │ grouped list         │  └────────────────────────────┘ │
├──────────────────────────────────────┤      │                      │  Replay 21:46:58                │
│ 13 clips · 5.2 GB             📁 ⟳ ⚙ │      │                      │  D:\Clips › Elden Ring › …     │
└──────────────────────────────────────┘      │                      │  Game  Kind  Saved  Size        │
                                              │                      │  [Open][Show in folder][Rename…]│
                                              ├──────────────────────┴─────────────────────────────────┤
                                              │ 13 clips · 5.2 GB                                📁 ⟳ ⚙ │
                                              └────────────────────────────────────────────────────────┘
```

- **row** hides the kind filter (and resets it to All, so no hidden filter is left on), turns the list into a
  sideways strip of cards (`QListView` in list mode with left-to-right flow and no wrapping; the vertical wheel
  scrolls it sideways), and has no group headers.
- **wide** shows `ClipPreview` for the current clip; entering it selects the first clip when none is selected.
- The toast sits above the footer (column, wide) or at the bottom of the rail (row).

## Widget tree

```
ClipDock : QWidget                                    ui/clip-dock
├─ StatusHeader          set_layout(column | rail | wide)            ui/status-header
│  ├─ GameAvatar         initials on the accent, or a gamepad         ui/dock-controls
│  ├─ GameTitle          "Now playing" / game name (painted, elided)
│  ├─ ToggleSwitch       Auto-sort
│  ├─ StatusPill         dot + "Replay Buffer on" + "started by Clip Atlas"
│  └─ QPushButton        Save replay
├─ BannerArea            rows are NoticeFrames                       ui/banner-area
├─ QLineEdit             search, leading search icon, clear button
├─ ChipBar               All / each game / Not sorted, with counts   ui/dock-controls
├─ SegmentBar            All / Replays / Recordings / Screenshots
├─ QStackedWidget        QListView (ClipListModel + ClipListDelegate) | EmptyState
├─ ClipPreview           wide only                                    ui/clip-preview
├─ DockToast             overlay child, in no layout                  ui/dock-toast
└─ footer                TextBlock summary + GhostButtons: Open clips folder, Refresh, Settings
```

## Theme rules (amending M3)

- Colours still come from the palette, which OBS 31 builds from its theme variables (`palette_*` in the theme,
  applied with `setPalette`; checked in `obs-app-theming.cpp`). Roles used: Base (the dock), Window (notice fills:
  pill, banners, toast), Button (chips, inputs, borders), Highlight / HighlightedText (accent, selection, New tag),
  Text, and the Disabled group's Text (muted; Yami sets it to its muted grey). Hover fills mix Base or Button with
  Text, so dark and light themes both work.
- **Semantic colours**: the theme's `text-success` and `text-warning` classes (in 31.0's Yami) give the status dot
  and the toast badge their green and amber. `dock-style` reads them from a polished probe label; a theme without
  them gets fixed fallbacks.
- **Painted, not styled**: on Windows OBS keeps the native style under its style sheet, which draws item-view
  selection in Windows colours, and its `QWidget` font rule overrides `setFont()` sizes. So rows, chips,
  segments, the switch, the pill, headings and icons are painted by the plugin from the palette and the widget's
  own font. The plugin still sets no style sheet. Plain `QPushButton`s and the `QLineEdit` stay theme-styled.
- **Icons**: stroke icons drawn by `dock-style` in palette colours (folder, refresh, settings, search, open, more,
  the three kinds, check, warning, save, clear, gamepad). The ⚙ glyph goes.

## Strings

Every new string is in `data/locale/en-US.ini` under `Dock.*`. The ones that compose:
`Dock.Count.One` / `Dock.Count.Many` ("%1 clips"), `Dock.Summary` ("%1 · %2"), `Dock.Toast.Filed` ("Filed in %1"),
`Dock.Toast.Failed` ("Couldn't sort %1"), `Dock.Footer.Truncated` ("Showing the newest %1 clips"). Month headers use
`QLocale::system()`'s month name and year; times of day use its short time format.

## Behaviour

| Part | Behaviour |
|---|---|
| Header | Game: avatar with `initials_of(name)` on Highlight, eyebrow `Dock.NowPlaying`; none: gamepad, `Dock.WaitingForGame`, `Dock.NoGame` dimmed. Pill: on (by us / by the user), stopped (with `Dock.Replay.WithGame` while auto start is on), off in OBS (warning colour). Save replay is enabled only while the buffer runs and calls `obs_frontend_replay_buffer_save()` (in the 31.0 header) after checking `obs_frontend_replay_buffer_active()`. |
| Chips | `Dock.Filter.All` with the index's clip count, then each `ClipIndex::games()` entry with its count, then `Dock.Game.Unfiled` when `has_unfiled()`. The choice is kept while its chip exists; otherwise All. |
| Kind filter | Sets `ClipQuery::kind` (new). |
| Groups | `day_group_of(local_date_of(modified), local_date_of(now))` (new, `core/clip-display`): Today, Yesterday, Previous 7 days, then one header per month. A header shows `Dock.Count` and the size. Rebuilt when the date changes (checked by the minute timer). Headers are rows with no item flags, so they can't be selected. |
| Rows | Picture 80 x 45 with a kind badge (no picture: a tile with the time); title from `parse_clip_title()` (new), as "<prefix or kind> <time><suffix>", else the stem; game and size below; time on the right (today and yesterday: time of day; previous 7 days: short weekday and time; older: day and short month). Hovering shows Open, Show in folder and More… (the right-click menu) in place of the time. |
| Cards (row layout) | Picture with the time and kind, New badge, title and game · size below. |
| Fresh clip | The clip filed most recently in this session shows New, and its row flashes once in the accent. |
| Toast | After a clip is filed (moved or copied): `Dock.Toast.Filed` with `folders_below(base, final_path)` (new) joined by " › ", and the title and size; Show reveals the file. After a failed sort: `Dock.Toast.Failed` and the reason; Show reveals the original. 5 s, paused while hovered; a new toast replaces the old one. |
| Preview (wide) | Picture at 640 px from `thumbnail_provider()` (its own small cache; the list's picture meanwhile), title, path, Game / Kind / Saved / Size, and Open, Show in folder, Rename…, Move to game…, Move to Recycle Bin… (the M3 actions). |
| Footer | `Dock.Count` and total size of the index (`Dock.Empty.Title` when empty), or `Dock.Footer.Truncated` when the last scan hit the cap. |
| Empty | No clips: `Dock.Empty.Title` and `Dock.Empty.Body`. Nothing matches: `Dock.Empty.NoMatch` and `Dock.Empty.NoMatchHint`. |

## Threads and lifetime

As M3. The preview's thumbnail answers hop with `run_on_ui()` and are guarded by a `QPointer` and a ticket, like
the model's. `stop()` also stops the toast, the flash and the preview's requests.
