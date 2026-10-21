# WP3.6 hand-over: `ui/clip-dock` assembly, right-click menu, registration and plugin wiring (worker)

## Summary

The clip browser dock, built to the contract in `src/ui/clip-dock.hpp` and `docs/handover/M3-dock-spec.md`, and wired into
`src/plugin-main.cpp`.

- **ClipDock**: the widget tree from the spec (StatusHeader, BannerArea, game combo and search box, a stack of list or
  empty message, and the footer with Open clips folder, Refresh and ⚙). Scans run on the dock's own TaskQueue. A result is
  applied only if its scan is still the newest, and only while the dock is alive and not stopped.
- **Clip actions**: a right-click menu (Open, Show in folder, Rename…, Move to game…, Move to Recycle Bin…). Double-click or
  Enter opens a clip, Delete recycles it after a question whose default is No, and F2 renames it.
- **register_clip_dock()** adds the dock with `obs_frontend_add_dock_by_id("clip-atlas-dock", …)`. If OBS refuses the id,
  the widget is deleted and nullptr is returned.
- **plugin-main.cpp**: the replay status callback, the dock registration (before the final frontend callback, and in its
  own try so a dock failure cannot fail the plugin load), and the dock's hooks in the game, settings, filing, frontend
  event and shutdown paths.

Nothing is committed. The changes are staged with `git add -A`.

## Files changed

| File | Change |
|---|---|
| `src/ui/clip-dock.cpp` | new, 1041 lines. `ClipDock`'s public functions, `ClipDock::Impl` (defined here; the header is unchanged, public and private), `register_clip_dock()` |
| `src/plugin-main.cpp` | `#include`s for the dock, shell-ops and thumbnails, and `<QPointer>`; file-scope `g_dock`; a third try in `on_game_changed()`; a try at the end of `apply_settings_change()`; a dock try before the save sound in `on_clip_sorted()`; the dock start after `start_replay_control()`, and a PROFILE_CHANGED branch, in `on_plugin_frontend_event()`; the dock's stop, and the thumbnail and Shell shutdowns, in `stop_all()`; the status callback and the dock registration in `start_plugin()` |
| `docs/handover/WP3.6.md` | this note |

Not touched: `src/ui/clip-dock.hpp`, `data/locale/en-US.ini`, `CMakeLists.txt`, and every other file.

## How it was tested

Commands were run in the worktree.

1. **Step 0**: `git merge --ff-only claude/pensive-hamilton-cbg1l4`, then `git log -1 --oneline` shows
   `17e3025 WP5.6: README, install guide and changelog`.
2. **Linux plugin, warnings as errors**: `cmake --preset ubuntu-ci-x86_64` (configure passes), then
   `cmake --build build_x86_64`. Exit 0, no warnings. The compile line for `clip-dock.cpp` includes `-Werror -Wextra`
   (`ninja -t commands`). The module links on Linux with 14 undefined `clip_atlas` symbols, from `nm -C
   build_x86_64/clip-atlas.so | grep " U " | grep clip_atlas`:
   - 11 from `ClipListModel` (`ctor`, `clip_at`, `index`, `query`, `refresh_ages`, `remove_clip`, `reset_clips`,
     `row_of`, `set_query`, `shutdown`, `upsert_clip`) and 1 from the `ClipListDelegate(QObject*)` constructor: **WP3.4**
     (`src/ui/clip-list-model.cpp`), not on this branch.
   - 2 from `thumbnail_provider()` and `ThumbnailProvider::shutdown()`: **WP3.2** (`src/platform/thumbnails-{win,stub}.cpp`),
     not on this branch.
   - `shell_ops()` and its methods are defined by `shell-ops-stub.cpp` on Linux, so they do not appear.
3. **Core unit tests**: `cmake -S tests -B build/tests && cmake --build build/tests && ctest --test-dir build/tests
   --output-on-failure`. Result: **343 of 343 passed**.
4. **Formatting**: `clang-format-19 --style=file --fallback-style=none --dry-run --Werror src/ui/clip-dock.cpp
   src/plugin-main.cpp` is clean. Then `git add -A && build-aux/check-format` reports "all 83 C/C++ and 16 CMake files are
   formatted."
5. **Locale**: all 26 `Dock.*` keys that `clip-dock.cpp` uses exist in `en-US.ini`, the four error keys passed as arguments
   included (checked by script). No key was added or substituted.
6. **Offscreen smoke test** (outside the repo, not committed; see the path below). It compiles the real `clip-dock.cpp`,
   `status-header.cpp`, `banner-area.cpp`, `ui-thread.cpp` and the core files the dock uses (clip-index, clip-layout,
   clip-routing, file-mover, game-state, name-resolver, settings-model, task-queue, remux-rules). Test doubles stand in for
   the OBS C API (`obs_queue_task` runs on the UI thread as OBS does), the Shell (asynchronous, on its own threads, with real
   renames, and a folder as the Recycle Bin), the settings store and dialog, and the list model (a minimal stand-in for
   WP3.4). Built with `-fsanitize=address,undefined` and run with `QT_QPA_PLATFORM=offscreen`:
   - **122 checks, 0 failed, no sanitizer report.** Covered: the first scan and the clip kinds; the combo items; the
     game and Not sorted filters; the search and both empty messages; a game whose folder disappears (the combo falls back to
     All games and the filter follows); `on_clip_sorted` (a moved clip appears with its exact kind; a failed move and a missing
     file change nothing); the replay banner with the auto-start setting on and off; the auto-sort switch writes the setting;
     the folder-missing and no-folder banners, open-folder enablement and the banner's Settings button; Refresh; Open clips
     folder; ⚙; an open failure (warning and WARNING log, list unchanged); rename (kind kept, old row removed); a bad name
     (no Shell call); rename onto an existing file (Shell refuses, warning, clip stays); move to game (own folder left out of
     the list, typed name trimmed, file lands in `<Game>/<YYYY-MM>` by its modified time); recycle (No changes nothing, Yes
     removes the row); the F2 and Delete shortcuts; show and hide; `stop()` (no scan, no filing, `banners()` is null); a dock
     deleted with a scan pending (three rounds); `register_clip_dock` with OBS refusing and then accepting the id.
   - **Mutation checks**, on a scratch copy, to confirm the checks can fail: (a) the filter fallback removed, which is caught
     at the first game-vanished check; (b) the move no longer removing the old row, which gives 4 failures in the move and
     after it. Several mistakes in the harness itself (widget lookups, a miscounted row, a shortcut test that depended on
     keyboard focus) were found and fixed on the way; none was in the dock.

Smoke harness: `/tmp/claude-0/-home-user-auto-clip-sorter/6c995450-a095-5948-9d9e-9af008631cf2/scratchpad/wp36-smoke/`
(`main.cpp`, `fakes.cpp`, `fake-model.cpp`, `fakes.hpp`, `build.sh`, and the run logs).

## Deviations from the brief, and why

1. **No-folder banner text** is `Dock.Banner.NoFolder`. The brief named no text key, and this is the locale's key for "no
   clips folder".
2. **Rename to the same name is a no-op**, and spaces at the ends are ignored. The brief's rule (`renamed_file_name` gives
   nullopt, so show `Dock.Error.BadName`) would tell the user that an unchanged name "can't be used".
3. **Folder names are compared case-insensitively** for the own-folder refusal and for leaving the clip's folder out of the
   Move list. Windows folder names ignore case. With an exact compare, typing `valorant` for a clip in `Valorant` would
   move it into its own folder, and the mover would rename it onto itself with a `(2)` suffix.
4. **`value().toString()`** in the brief is `toString()`. In Qt 6, `QVariant::value()` needs a template argument.
5. **When a scan starts**: the folder, the replay prefix, the monthly-folders setting and the unsorted-folder name are
   compared with the last scan, in `apply_settings`, `on_profile_changed` and `showEvent`. Any change starts a scan. The
   brief's `showEvent` rule checked only the base, so this is a superset.
6. **Extras**: `refresh_ages()` runs on show, so the ages are current at once. The game combo uses
   `AdjustToMinimumContentsLengthWithIcon` with 16 characters, so a long game name cannot widen the dock. The open-folder
   button is disabled until the first scan as well as while the base is empty. ⚙ uses the text glyph (U+2699) with
   `ToolButtonTextOnly`, which the brief allowed.
7. **Context menu**: the chosen action is read from `QMenu::exec()`'s return value, not from `triggered`. The menu has no
   parent, so the list cannot delete it. Each Shell or mover action starts after the menu has closed.
8. **Deferred work reaches the dock only through the weak_ptr, on the UI thread.** Every slot, Shell callback, mover callback
   and queued task calls `with_live()`, which locks the weak_ptr and catches exceptions. The `QPointer` stays inside the
   shared state, which only the UI thread reads. The brief's version captured a `QPointer` in lambdas that run on other
   threads. Qt does not document that as safe, so the pointer never leaves the UI thread.
9. **The Impl is defined in `clip-dock.cpp`.** The header's pimpl is unchanged.
10. **One TaskQueue** runs both scans and describes, as the brief says. Its first-in, first-out order means a describe queued
    after a scan is applied after that scan, and a later scan includes the new file.

## Known gaps and risks

1. **Windows link depends on WP3.2 and WP3.4.** The 14 symbols above must be defined before the Windows build links. The
   Linux module links anyway, so the Linux check does not show this.
2. **EXIT can wait.** `ClipDock::stop()` joins the dock's queue, so a scan still running delays EXIT by the scan time (a slow
   or network folder). `stop_all()` calls `shell_ops().shutdown()`, which waits for a running Shell call. An Explorer prompt
   (such as Windows' permanent-delete warning) can therefore hold EXIT until the user answers.
3. **The shared mover delays auto-sort.** "Move to game…" uses the plugin's `FileMover`, the same one that auto-sort uses. A
   large cross-drive move started from the dock queues ahead of sorting and delays it, because the worker is first in, first
   out. A second mover for the dock would avoid this.
4. **The 5000-clip cap is silent.** `ScanResult::truncated` is ignored, so clips beyond the index's cap are not listed and
   nothing says so.
5. **A partly failed move leaves its old row.** When the source vanishes mid-copy, the result is `failed` with a kept partial.
   The old row stays until the next scan. The partial is never listed, because `is_clip_file` excludes it.
6. **Not run on Windows or with OBS 31.** The Linux build uses libobs 30.0.2 and Qt 6.4.2 headers (the environment's). The
   Shell calls, the thumbnails, the real list model, the menu position (viewport coordinates, as the brief says), OBS's
   theme and the real dock behaviour inside OBS are all unverified.
7. **Keyboard delivery was not exercised.** The offscreen platform gives no keyboard focus. The F2 and Delete shortcuts were
   checked by key and context, then activated, which tests the handlers but not the key path.
8. **The model and Shell in the harness are doubles.** The real list model (WP3.4) should be run through the harness once it
   merges, since the harness only checks its contract.

## plugin-main.cpp notes

- The dock is optional: `register_clip_dock()` is in its own try, and a failure is logged. The plugin keeps working without it.
- The replay status callback is set before the dock is registered, and the final frontend callback stays last.
- Each new hook is in its own try and checks `!g_stopped && g_dock`.
- Two existing comments were extended with a sentence or bullet about the dock. No existing line was changed otherwise.
  One existing line in `on_game_changed()` (over 120 columns with tabs at 8, and already so in HEAD) was left as it was.

## Questions for the orchestrator

1. Should "Move to game…" use the plugin's shared `FileMover`, as the brief says, or its own mover? (Risk 3.)
2. Are deviations 1 to 3 acceptable: the no-folder text key, the same-name rename no-op, and case-insensitive folder names?
3. Is it acceptable that EXIT can wait for an Explorer prompt, through `shell_ops().shutdown()` (risk 2)? If not, the Shell
   shutdown needs a timeout, which is WP3.3's design.
4. Should the list show a notice when the 5000-clip cap is reached? The spec does not ask for one.
5. WP3.4 and WP3.2 must land before the Windows link is clean. Once they do, should the harness be rerun against the real list
   model?

## Orchestrator integration notes

- Read the whole diff; only the WP's files changed. `plugin-main.cpp`: every dock call is in its own try and checks
  `!g_stopped && g_dock`; the status callback is set before the dock exists; the final frontend callback stays last.
- Gates: plugin build with warnings as errors; with WP3.2 and WP3.4 on the branch, the Linux module has no undefined
  `clip_atlas` symbols, so the Windows link has everything; 343/343 unit tests; check-format clean.
- Answers: (1) the shared `FileMover` stays (a dock move is rare and user-started; a delay to auto-sort is visible
  only for a large cross-drive move). (2) Deviations 1-3 accepted. (3) Accepted: EXIT waits only while Windows'
  own delete question is open. (4) No notice for the 5000 cap in M3 (listed under the checklist's known limits).
  (5) Re-run against the real list model and thumbnail stub at 4687bb0, under ASan/UBSan: 122 checks, 0 failed, no
  sanitizer or leak reports.
