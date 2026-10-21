# Clip Atlas

*Every clip, mapped to its game.*

Clip Atlas is a native OBS Studio plugin for Windows. It sorts your replays, recordings and
screenshots into per-game folders automatically, with no Python install and no setup.

> **Status:** in development. The original Python script lives in [`legacy/`](legacy/) and is
> deprecated. See [`docs/PLAN.md`](docs/PLAN.md) for the roadmap.

## Features (planned for v1)

- **Zero setup.** Clip Atlas finds every Game Capture and Window Capture on its own and uses
  OBS's recording folder by default.
- **Accurate game detection.** Folders are keyed by the game's executable and shown with a
  friendly name, so a game whose window title changes still lands in one folder.
- **Replays, recordings and screenshots** are all sorted.
- **Auto Replay Buffer.** Starts when a game is hooked and stops when it closes, after a short
  grace period.
- **Clip browser dock** inside OBS, with thumbnails and quick actions.
- **Game rules.** Rename games, or ignore apps you don't want sorted.

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

## Development

The core logic is plain C++17 and is unit-tested without OBS:

```sh
cmake -S tests -B build/tests
cmake --build build/tests
ctest --test-dir build/tests --output-on-failure
```
