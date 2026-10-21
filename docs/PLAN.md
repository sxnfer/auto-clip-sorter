# Clip Atlas — plan

Converting the `legacy/rb_event_driven.py` OBS script into a native OBS plugin that installs
without friction and ships a modern UI built from OBS's own Qt interfaces.

## Decisions

| Topic | Decision |
|---|---|
| Name / ID | **Clip Atlas** / `clip-atlas` |
| Platforms | Windows only (game-capture hook signals are Windows-only) |
| Minimum OBS | 31 (build against the oldest supported libobs) |
| Stack | C++17 + Qt 6, based on `obsproject/obs-plugintemplate` |
| License | GPL-2.0-or-later (required because the plugin links libobs) |
| Folder key | Game executable, shown with a friendly name |
| Folder layout | `<base>/<Game>/<YYYY-MM>/<file>`, monthly folders on by default and optional |
| Replay Buffer | Auto start/stop on by default, 10 s grace period, only stops what it started |
| Signing | Unsigned for now (document SmartScreen; SignPath Foundation later) |
| Updates | Check GitHub Releases at most once a day; can be turned off |
| Python script | Kept in `legacy/` and deprecated: logs a warning, no new features |
| Save sound | Optional (off by default): a short "Replay saved" voice clip plays through Windows when a clip has been saved and sorted, for single-monitor users. Users can pick their own `.wav` and set the volume. Not kept out of Desktop Audio capture, so the help text warns it can be heard on stream and in later clips. The bundled default (ElevenLabs) ships only with a licence that allows redistribution |

v1 features beyond the script: clip browser dock, sorting recordings and screenshots, game
name rules, and watching several sources at once.

## Architecture

```
src/
  plugin-main.cpp          obs_module_load/unload; registers dock, Tools menu, hotkeys
  core/
    clip-layout            destination folder + collision naming          [done]
    source-tracker         auto-discovers capture sources, hooked/unhooked, active game
    name-resolver          rules → bundled games.json → exe ProductName → title → exe stem
    clip-sorter            replay/recording/screenshot events → move jobs
    file-worker            background moves, cross-drive copy, retry on locked files
    replay-controller      start/stop with grace period
    settings               JSON in obs_module_config_path()
    clip-index             in-memory index feeding the dock
    update-checker         WinHTTP → GitHub Releases API
  ui/
    clip-dock, settings-dialog, rules-editor, thumbnail-provider
data/  locale/en-US.ini, games.json
installer/  Inno Setup script → %ProgramData%\obs-studio\plugins\clip-atlas\
tests/  Catch2 unit tests for core logic (no OBS needed)
```

**Rules**
- Plugin state and Replay Buffer calls only on the UI thread. File I/O never on the UI thread.
- Sources are found by event (global `source_create`/`source_destroy`/`source_rename`,
  scene-collection events) and held by weak reference. There is no polling or retry loop.
- Only `hooked`/`unhooked` drive detection. `show`/`hide` are not used because they also fire
  for previews and projectors.
- The last-seen game is remembered with a timestamp, so a save during the grace period still
  goes to that game's folder.
- Recordings: respect auto-remux (wait for the `.mp4`) and split files.

## UI

- **Dock:**
  - status header (current game, Replay Buffer pill, auto-sort toggle)
  - banners for setup problems, a still-loaded legacy script, and updates
  - clip list with a game filter, search, thumbnails (Windows Shell API) and a right-click menu
  - footer with "Open clips folder" and ⚙ for settings
- **Settings dialog** with OBS-style sidebar pages: General, Detection, Replay Buffer, Games,
  Advanced.

## Milestones

| # | Milestone | What it delivers |
|---|---|---|
| 0 | Scaffold | Template, `buildspec.json` pinned to OBS 31, CI green, DLL loads, LICENSE |
| 1 | Feature parity | Source tracking, name resolution, replay sorting, Replay Buffer automation, settings |
| 2 | More capture types | Recordings (remux and split files) and screenshots |
| 3 | Dock | Status, banners, clip index, thumbnails, right-click actions |
| 4 | Games and settings | Rules editor, `games.json`, full settings dialog, import from the legacy script |
| 5 | Release | Inno Setup installer + zip, tag-driven releases, update checker, docs, beta |
| Later | Optional | Code signing, Display Capture foreground-window fallback, macOS |
