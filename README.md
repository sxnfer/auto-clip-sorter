# Clip Atlas

*Every clip, mapped to its game.*

Clip Atlas is a native OBS Studio plugin for Windows. It sorts your replays, recordings and
screenshots into per-game folders automatically, with no Python install and no setup.

> **Status:** version 1.1.0 is out: download it from the
> [Releases page](https://github.com/sxnfer/auto-clip-sorter/releases/latest) (see
> [`docs/install.md`](docs/install.md)). The built-in games list, the rules editor and the
> full settings pages (milestone M4) come later. The original Python script lives in
> [`legacy/`](legacy/) and is deprecated. See [`docs/PLAN.md`](docs/PLAN.md) for the roadmap.

## What it does

- **Sorts clips by game.** Saved replays, recordings and screenshots are moved into a folder
  for the game you were playing. Clips made while no game is detected go to an `Unsorted`
  folder.
- **Finds your games by itself.** Clip Atlas watches every Game Capture and Window Capture
  source in OBS and notices when a game is being captured. You don't add games one by one.
- **Starts and stops the Replay Buffer for you.** It starts when a game is detected and stops
  when the game closes, after a grace period of 10 seconds by default. It only stops a Replay
  Buffer that it started itself.
- **Handles recordings.** A recording is filed when you stop it, including every part of a
  split recording and the `.mp4` copy that auto-remux makes. Optionally, tick **File each
  recording under the game played longest** (off by default) to file a recording under the game
  you played for the longest time during it.
- **Sorts screenshots** taken in OBS the same way.
- **Choose what to sort.** Replays, Recordings and Screenshots can each be turned off in
  **Tools → Clip Atlas Settings…**.
- **Optional save sound (off by default).** Clip Atlas can play a short `.wav` file that you
  choose, once a replay has been saved and sorted. Windows plays the sound through its default
  output, which OBS's Desktop Audio captures, so viewers and later clips can hear it. This
  release includes no sound file.
- **Clip browser dock.** Open it with **Docks → Clip Atlas**. It follows your OBS theme and fits
  wherever you put it: a list in a side column, a strip of cards in the short bottom row, or a
  list with a large preview when you float it big. It shows:
  - the game being played, the Replay Buffer's state, an **Auto-sort** switch and a
    **Save replay** button;
  - an activity pill that shows what Clip Atlas is doing with your latest save, with each
    file's steps and the recent saves a click away;
  - banners when something needs attention, such as a missing clips folder;
  - your clips grouped by day (Today, Yesterday, Previous 7 days, then by month), with
    thumbnails from Windows, game chips with counts, a type filter (Replays, Recordings,
    Screenshots) and a search box (**Ctrl+F**);
  - a note after each save that says where the clip was filed, with **Show** to open its folder;
  - actions on a clip, on hover or with a right-click: **Open**, **Show in folder**, **Rename…**,
    **Move to game…** and **Move to Recycle Bin…**. Clip Atlas never deletes a clip for good
    itself.
- **Auto-sort hotkey.** In OBS, open **Settings → Hotkeys** and bind a key to
  **Toggle Clip Atlas auto-sort** to turn auto-sorting on or off.
- **Update check.** Once a day, Clip Atlas asks GitHub whether a newer version exists and tells
  you. It never downloads or installs anything by itself. You can turn the check off in
  **Clip Atlas Settings…**.

### Planned, not in this release

- **Built-in games list (M4).** Until then, a game's folder is named after its window title,
  with version numbers and suffixes such as "- Steam" removed.
- **Rules editor (M4).** Rename a game's folder, or ignore an app so its clips go to the
  Unsorted folder.
- **Full settings pages (M4).**
- **Importing settings from the Python script (M4).**

## Folder layout

```
<Clips folder>/
├── Fortnite/
│   ├── 2026-09/
│   │   └── Replay 2026-09-28 21-14-03.mkv
│   └── 2026-10/
│       ├── Replay 2026-10-20 19-02-44.mkv
│       └── Screenshot 2026-10-20 19-05-10.png
├── Elden Ring/
│   └── 2026-10/
└── Unsorted/        ← clips captured while no game was detected
    └── 2026-10/
```

- **Monthly folders** (`YYYY-MM`) are on by default and can be turned off to keep each game
  folder flat. The numeric format sorts chronologically in Explorer.
- A clip's month is the local date it was saved.
- File names are left as OBS created them. A clash becomes `Replay (2).mkv`.

## Install

You need Windows 10 or 11 (64-bit) and OBS Studio 31 or newer (64-bit).

Follow [docs/install.md](docs/install.md). It covers the installer, the zip file, the first
start, and the warning that Windows may show for the installer, which is not code-signed yet.

## Update

Download the new installer and run it. Your settings are kept. The
[updating steps](docs/install.md#updating) also cover the zip.

## Uninstall

Open **Settings → Apps → Installed apps**, choose **Clip Atlas (OBS plugin)** and select
**Uninstall**. Your settings and your clips are not deleted. See
[uninstalling](docs/install.md#uninstalling) for the details.

## Troubleshooting

If the plugin does not appear, clips land in the wrong folder, or the Replay Buffer does not
start, see [Troubleshooting](docs/install.md#troubleshooting).

## Changes

[CHANGELOG.md](CHANGELOG.md) lists what this release adds, and its known limitations.

## Development

The core logic is plain C++17 and is unit-tested without OBS:

```sh
cmake -S tests -B build/tests
cmake --build build/tests
ctest --test-dir build/tests --output-on-failure
```
