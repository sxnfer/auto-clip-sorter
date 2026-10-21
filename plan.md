# Clip Atlas — build plan (subagent-driven)

This is the plan for building Clip Atlas. One **orchestrator** (Opus 5.5) runs it, and
**worker agents** (Haiku 5.5 at max reasoning) implement the work packages (WPs).

- **Product decisions** (what we're building and why) live in [`docs/PLAN.md`](docs/PLAN.md).
- **Coding conventions** every agent must follow live in [`CLAUDE.md`](CLAUDE.md).
- **The prompt that starts the orchestrator** lives in
  [`docs/ORCHESTRATOR_PROMPT.md`](docs/ORCHESTRATOR_PROMPT.md).

The orchestrator keeps the **Status** column of every WP table up to date. That column is
the single record of progress.

---

## 1. Roles

| Role | Model | Responsibilities |
|---|---|---|
| Orchestrator | Opus 5.5 | Owns the plan. Writes the interface headers. Writes every worker brief. Reviews and merges. Runs the quality gates. Talks to CI and GitHub. Writes milestone hand-over notes for the user. Writes the code itself only for interfaces, small fix-ups, and work a worker failed twice. |
| Worker | Haiku 5.5, effort `max` | Implements exactly one WP inside its file ownership. Writes tests for it. Runs the local checks. Writes a hand-over note. |
| Reviewer | Haiku 5.5, effort `max` | Optional second opinion. Reviews another worker's diff against its brief with a skeptical eye. Used for WPs marked ★ (concurrency, file I/O, anything that deletes or moves user files). |
| User | Human | Tests each milestone in real OBS on Windows, which no agent can do. Approves pull requests. Answers questions the orchestrator escalates. |

Workers start with no context. **A brief must be self-contained.** Workers never see this
conversation, only the brief, the repo, and `CLAUDE.md`.

---

## 2. Development constraints (read before planning any WP)

1. **The dev container runs Linux. The product targets Windows.** Code is therefore layered
   so that most of it can be verified on Linux:
   - `src/core/` holds pure C++17. It never includes OBS, Qt or Win32 headers. It is
     unit-tested on Linux with Catch2. **Most of the logic lives here.**
   - `src/platform/` holds the Win32 implementations (`*-win.cpp`) behind small interfaces,
     plus stubs (`*-stub.cpp`) so non-Windows builds still compile.
   - `src/obs/` holds the thin OBS glue: signals, frontend events, UI-thread hops. It is
     compile-checked only and kept as thin as possible.
   - `src/ui/` holds the Qt widgets. They are compile-checked and then tested by hand.
2. **Only CI and the user can verify Windows.** CI builds on GitHub Actions with
   `windows-2022`. The orchestrator checks CI results with the GitHub MCP tools.
   "Done" for any Windows-only code means **CI is green** and **the item is on the user's
   manual test checklist**.
3. **Building the OBS-dependent code on Linux.** WP0.1 finds out whether the container can
   build against libobs, obs-frontend-api and Qt 6, either through the template's Ubuntu
   preset or the OBS PPA. If it can, the orchestrator compile-checks the glue and UI code
   locally before pushing. If it can't, CI is the only compile check for those layers, and
   the orchestrator pushes once per WP group rather than once per WP.
4. **No new third-party dependencies** beyond the ones `CLAUDE.md` allows, unless the
   orchestrator approves them and records the reason here.

---

## 3. Workflow

### 3.1 Per milestone
1. Create the branch `claude/m<N>-<slug>` from the latest `main`. (M0 starts from
   `claude/clip-atlas-foundation` if that branch hasn't been merged yet.)
2. **Write the interfaces first.** The orchestrator writes or updates the headers for every
   WP in the milestone, then commits them. Workers code against these fixed contracts, which
   is what lets them run in parallel. A worker may not change a header it doesn't own. To
   ask for a change, it says so in its hand-over note.
3. Dispatch the WPs in **parallel groups**, in the order of the dependency table. Run the
   WPs within a group at the same time, each in its own worktree
   (`isolation: "worktree"`). Two WPs in one group never share a file they're allowed to
   change.
4. Integrate each WP as it comes back (§3.3).
5. **Milestone gate** (§3.4), then open a pull request against `main` and **stop**. Report
   to the user and wait for their manual test results before starting the next milestone.

### 3.2 Worker brief template
Every brief the orchestrator sends to a worker uses this shape:

```
You are implementing work package <ID> "<title>" for Clip Atlas, a native OBS Studio plugin
(C++17/Qt6, Windows target). You are one of several agents; stay strictly inside your scope.

READ FIRST: CLAUDE.md, plan.md §<WP section>, <interface headers>, <related files>.

GOAL: <one paragraph, the behavior to implement>

YOU OWN (may create/modify): <exact paths>
READ-ONLY (must not modify): everything else, especially <headers you implement>.

CONTRACT: <the functions/classes from the header you must implement, plus behavior notes,
edge cases, and error handling expectations>

ACCEPTANCE CRITERIA:
- <observable, testable statements>
- Unit tests in tests/<file> cover: <listed cases>
- `cmake --build build/tests && ctest --test-dir build/tests --output-on-failure` passes
- clang-format clean: `clang-format --dry-run --Werror <your files>`
- <compile check command if the layer can be built locally>

DO NOT: add dependencies, touch files outside YOU OWN, change public headers, leave TODOs
without listing them in your handover, commit or push.

FINISH BY writing docs/handover/<ID>.md with: summary, files changed, how it was tested
(commands and results), deviations from the contract and why, known gaps or risks, and
questions for the orchestrator. Then reply with that note's content.
```

### 3.3 Integration loop (per returned WP)
1. Read the hand-over note, then the **whole diff**. Reject anything outside the WP's
   ownership.
2. Run the gates: build, the full test suite, clang-format, and the local compile check
   if one is available.
3. Review the diff with a skeptical eye against the contract and the threading and UTF-8
   rules in `CLAUDE.md`. For ★ WPs, also send the diff and the brief to a Reviewer agent.
4. **If something fails**, send the same worker (SendMessage, so it keeps its context) a
   precise list of the failures. Allow at most **2 fix rounds**. After that the
   orchestrator fixes it directly and records why in the hand-over note.
5. Merge the worktree, then commit with the message `WP<ID>: <title>`, ending with the
   session's attribution trailer.
6. Mark the WP ✅ in the table below and record any follow-ups it raised.

### 3.4 Milestone gate
- Every WP in the milestone is ✅, and there are no unresolved questions in the hand-over
  notes.
- The local unit tests pass, and Windows CI is green on the pushed head.
- `docs/testing/M<N>.md` exists. It's a manual test checklist for the user with numbered
  steps, the expected result for each, and a way to record pass or fail.
- The **Status** column and **Decision log** here are up to date.
- The pull request is open, and its description summarizes the WPs, the CI result, and
  the manual tests the user needs to run.

---

## 4. Target source layout

```
CMakeLists.txt, CMakePresets.json, buildspec.json, cmake/, .github/workflows/   (template)
src/
  plugin-main.cpp
  core/       clip-layout ✅, name-resolver, game-state, replay-policy, settings-model,
              file-mover, recording-tracker, clip-index, scripts-import, version
  platform/   process-info{.hpp,-win.cpp,-stub.cpp}, thumbnails{…}, http{…}, shell-ops{…}
  obs/        source-tracker, frontend-events, replay-controller, settings-store,
              ui-thread, hotkeys
  ui/         clip-dock, status-header, banner-area, clip-list-model, settings-dialog,
              rules-editor
data/         locale/en-US.ini, games.json
installer/    clip-atlas.iss
tests/        test-*.cpp (Catch2), fixtures/
docs/         PLAN.md, handover/, testing/
legacy/       deprecated Python script (do not modify)
```

---

## 5. Work packages

Legend: **Grp** = parallel group within the milestone · ★ = needs a Reviewer agent ·
**Orch** = done by the orchestrator, not a worker.

### M0 — Scaffold

| ID | Title | Grp | Depends | Status |
|---|---|---|---|---|
| WP0.1 | Bring in obs-plugintemplate | A | — | ✅ |
| WP0.2 | Plugin entry point, locale, LICENSE | B | 0.1 | ✅ |
| WP0.3 | CI: unit tests + format check | B | 0.1 | ✅ |
| WP0.4 | M1 interface headers | C (Orch) | 0.2 | ✅ |

- **WP0.1 — Template import.**
  - Copy the current `obsproject/obs-plugintemplate` into the repo root.
  - Set the plugin name to `clip-atlas`, the display name to "Clip Atlas", and the author.
  - Pin `buildspec.json` to OBS **31.0.x** and its matching deps and Qt 6.
  - Keep the Windows preset. Keep the Ubuntu preset for compile checks only. Drop macOS.
  - Keep the template's GitHub Actions workflows, trimmed to Windows plus Ubuntu.
  - Find out whether the container can configure the Ubuntu preset (constraint 3) and
    write the answer in the hand-over note.
  - *Accept:* the Windows build in CI produces `clip-atlas.dll`, and the existing `tests/`
    still build.
- **WP0.2 — Entry point.**
  - `src/plugin-main.cpp` with `obs_module_load` and `obs_module_unload`. Loading logs
    `[clip-atlas] loaded v<version>`.
  - `data/locale/en-US.ini` with a starter set of strings.
  - The GPL-2.0-or-later `LICENSE` file.
  - *Accept:* the build passes on CI, and the user's checklist confirms the log line shows
    up in OBS.
- **WP0.3 — CI tests.** Add a job that builds and runs `tests/` on Ubuntu and Windows, and
  wire in the clang-format check. *Accept:* a deliberately failing test (on a throwaway
  commit, reverted afterwards) turns CI red.
- **WP0.4 — Interfaces (Orch).** Write the headers for every M1 WP:
  `name-resolver`, `game-state`, `replay-policy`, `remux-rules`, `settings-model`, `file-mover`,
  `process-info`, `ui-thread`, `source-tracker`, `frontend-events`, `replay-controller`
  and `settings-store`. Each header has doc comments that are precise enough to implement
  from.

### M1 — Feature parity, done right

| ID | Title | Grp | Depends | Status |
|---|---|---|---|---|
| WP1.1 | `core/name-resolver` | A | 0.4 | ✅ |
| WP1.2 | `core/game-state` ★ | A | 0.4 | ✅ |
| WP1.3 | `core/replay-policy` + `core/remux-rules` | A | 0.4 | ✅ |
| WP1.4 | `core/settings-model` | A | 0.4 | ✅ |
| WP1.5 | `core/file-mover` ★ | A | 0.4 | ✅ |
| WP1.6 | `obs/ui-thread` + `obs/settings-store` | B | 1.4 | ✅ |
| WP1.7 | `obs/source-tracker` ★ | B | 1.2 | ✅ |
| WP1.8 | `obs/frontend-events` + replay sorting wiring | C | 1.1, 1.5, 1.6, 1.7 | ✅ |
| WP1.9 | `obs/replay-controller` | C | 1.3, 1.6, 1.7 | ✅ |
| WP1.10 | Minimal settings dialog (General page) | C | 1.6 | ✅ |

- **WP1.1 name-resolver.** Resolves a game name from `{executable, window title,
  product name (optional)}`. It tries, in order:
  1. a user rule
  2. the bundled map
  3. the product name, unless it's on the generic blocklist ("Unreal Engine", "Unity",
     "Java(TM) Platform SE binary", …)
  4. the cleaned window title (strip version strings like `1.21.4` and trailing
     `- Steam`-style suffixes)
  5. the exe name without `.exe`

  It returns a stable **folder key** (the lowercased exe) separately from the
  **display name**, and also reports whether the game is ignored. Data comes in already
  parsed (`std::map`s). Matching is case-insensitive on the exe. Fully unit-tested.
- **WP1.2 game-state ★.** A pure state machine for which game is current, driven by
  `hooked(source_id, info)`, `unhooked(source_id)`, `source_live(source_id, bool)` and
  `source_removed(source_id)` events, with a clock injected so tests control time.
  - If several sources are hooked, the one live in the Program output wins. Otherwise the
    most recently hooked one wins.
  - `game_for_save(now)` returns the current game, or the last game seen if it was within
    the grace period. Otherwise it returns empty, meaning Unsorted.
  - Tests cover overlapping hooks, a source removed while hooked, and grace-period edges.
- **WP1.3 replay-policy.** A pure decision function that takes the game state, the
  settings, whether the Replay Buffer is active and whether we started it, and returns
  start, stop or nothing.
  - **Only stop what we started.**
  - A stop happens only after the grace period. A game hooking again during the grace
    period cancels the pending stop.
  - Unit-tested.
  - Also implements `core/remux-rules` (`expected_remux_path`), which mirrors OBS 31.0's
    `OBSBasic::AutoRemux()` naming. Unit-tested.
- **WP1.4 settings-model.** A plain struct with every setting from `docs/PLAN.md`, its
  defaults, validation and clamping, and a `schema_version` with a migration hook.
  Serialization goes through a tiny key/value interface, so `core/` stays free of OBS.
  Unit-tested.
- **WP1.5 file-mover ★.** Moves files on a worker thread.
  - Run it as a job queue on one `std::thread`, with clean shutdown that waits for or
    cancels jobs.
  - Uses `clip-layout`'s `destination_dir` and `unique_destination`.
  - On the same volume it uses `rename`. Across volumes it copies, checks the size, then
    deletes the original. It never deletes the source unless the copy is verified.
  - Retries a locked file with backoff up to about 30 s.
  - It also handles a "wait until the file is stable" job (size and mtime unchanged for
    N ms), which M2's remux handling needs.
  - Each result goes to a callback: success with the final path, or failure with a reason
    and the clip left where it was.
  - Tests use temporary directories and cover collisions, a missing source, a read-only
    destination, and shutting down while a job is queued.
- **WP1.6 ui-thread + settings-store.**
  - `run_on_ui(std::function)` built on `obs_queue_task(OBS_TASK_UI, …)`.
  - The settings store loads and saves `settings-model` as JSON at
    `obs_module_config_path("settings.json")`, writing to a temporary file and renaming it
    over the old one so a crash can't corrupt it.
- **WP1.7 source-tracker ★.**
  - Connects to the global `source_create`, `source_destroy` and `source_rename` signals
    and to the scene-collection changing/changed events.
  - Automatically tracks every `game_capture` and `window_capture` source, or only the
    user's chosen list.
  - Wires up each source's `hooked`/`unhooked` signals (calldata `title`, `class`,
    `executable`) and `activate`/`deactivate`, and feeds the results into `game-state`
    on the UI thread.
  - Holds weak references only. Disconnects everything when unloading.
  - **Confirm in OBS 31 source** that `window_capture` emits the hook signals, and record
    the answer in the hand-over note.
- **WP1.8 frontend-events.**
  - On `REPLAY_BUFFER_SAVED`, call `obs_frontend_get_last_replay()`, then
    `game_for_save`, then `name-resolver`, then a `file-mover` job.
  - OBS auto-remuxes a saved replay right after `REPLAY_BUFFER_SAVED`. When
    `expected_remux_path()` says a remux will happen, wait for the remuxed file to be stable,
    then move both files.
  - Log the result. Expose a `clip_sorted` callback for the dock in M3.
- **WP1.9 replay-controller.**
  - Turns `replay-policy` decisions into `obs_frontend_replay_buffer_start/stop` calls on
    the UI thread.
  - Tracks whether we started the buffer.
  - Detects when the Replay Buffer is disabled in Output settings and exposes that as a
    status for the banners in M3.
- **WP1.10 Minimal settings dialog.**
  - A Qt dialog reached through Tools → "Clip Atlas Settings…".
  - It has one page, General: clips folder (defaults to OBS's recording path), monthly
    folders, unsorted folder name, and the Replay Buffer auto start/stop toggles and grace
    period.
  - It follows OBS's theme, with no hard-coded colors and every string from
    `obs_module_text`.

### M2 — Recordings and screenshots

| ID | Title | Grp | Depends | Status |
|---|---|---|---|---|
| WP2.0 | Interface headers for M2 (+ `core/clip-routing`, save-sound settings fields) | Orch | M1 | ✅ |
| WP2.1 | `core/recording-tracker` ★ | A | 2.0 | ⬜ |
| WP2.2 | Recording glue: `obs/recording-events` + wiring (stop, remux, split) ★ | B | 2.1, 2.3, 2.5 | ⬜ |
| WP2.3 | `obs/clip-filer` + screenshot sorting in `obs/frontend-events` | A | 2.0 | ✅ |
| WP2.4 | General page: "What to sort" and "Save sound" groups | A | 2.0 (links with 2.5) | ✅ |
| WP2.5 | Save sound: `core/wav-volume`, `platform/sound-player`, wiring | A | 2.0 | ✅ |

File ownership (no two WPs in a group share a file):

| WP | Owns |
|---|---|
| 2.1 | `src/core/recording-tracker.cpp`, `tests/test-recording-tracker.cpp` |
| 2.2 | `src/obs/recording-events.cpp`, `src/plugin-main.cpp` (after 2.5) |
| 2.3 | `src/obs/clip-filer.cpp`, `src/obs/frontend-events.cpp` |
| 2.4 | `src/ui/settings-dialog.{hpp,cpp}` (the header's private members only), `data/locale/en-US.ini` |
| 2.5 | `src/core/wav-volume.cpp`, `tests/test-wav-volume.cpp`, `src/platform/sound-player-{win,stub}.cpp`, `src/plugin-main.cpp` |

- **WP2.0 (Orch)** writes `core/recording-tracker.hpp`, `core/wav-volume.hpp`,
  `platform/sound-player.hpp`, `obs/clip-filer.hpp`, `obs/recording-events.hpp`, and
  updates `obs/frontend-events.hpp` and the settings dialog's doc comment. It also
  implements `core/clip-routing` (`ClipKind`, `sorting_enabled`, `ClipGame`,
  `clip_game_for`, `plays_save_sound`) and the three save-sound settings fields, so no
  group-A WP has to share `settings-model`. CMake links `winmm` on WIN32.
- **WP2.1** is pure logic for recording sessions (`RecordingTracker`).
  - The game is decided when the recording **starts** (`game_for_save`, so the grace
    period applies). If the user enables "dominant game", it's the game current for the
    longest total time over the recording; time with no game doesn't count, and with no
    game at all it falls back to the start game.
  - It collects every file of a split recording: the first from
    `obs_frontend_get_last_recording()` at `RECORDING_STARTED`, then each `next_file`
    of the output's `file_changed` signal. A session id keeps a late signal out of the
    next recording.
- **WP2.2** wires up `RECORDING_STARTED`, `RECORDING_STOPPED` and the recording
  output's `file_changed` signal, and forwards game changes for the dominant-game clock.
  At stop it hands every part, each with its `expected_remux_path`, to a `ClipFiler`,
  which waits for each remuxed copy before moving the pair. The AutoRemux keys are read
  through `read_remux_config()` (`[Video] AutoRemux`, `[Output] Mode`,
  `[AdvOut] RecType`, `[SimpleOutput|AdvOut] RecFormat2`, confirmed against OBS 31.0).
- **WP2.3** moves M1's replay filing (stable wait, remux wait, move, report) out of
  `frontend-events.cpp` into a generic `ClipFiler`, keeps replays working through it,
  and adds `SCREENSHOT_TAKEN` with `obs_frontend_get_last_screenshot()`.
- **WP2.4** adds the "What to sort" toggles (replays, recordings, screenshots, all on by
  default, plus the dominant-game option) and the "Save sound" group to the General page.
- **WP2.5** plays an optional sound once a **replay** has been saved and sorted (the
  `clip_sorted` callback with status moved or copied, never on failure; once per
  `save_id`, so a remuxed pair plays once).
  - `platform/sound-player`: a worker thread reads the `.wav`, `core/wav-volume`
    scales its samples in memory, and `PlaySoundW(image, nullptr, SND_MEMORY | SND_ASYNC |
    SND_NODEFAULT)` plays it. The stub plays nothing.
  - Settings: `play_save_sound` (off by default), `save_sound_path` (empty means no
    sound) and `save_sound_volume` (0–100, default 80). The General page gets a Test
    button and help text saying the sound can be heard in Desktop Audio captures.
  - No sound file is bundled until the user confirms the ElevenLabs WAV's licence allows
    redistribution. Until then the feature only uses a user-chosen file.

### M3 — Clip browser dock

| ID | Title | Grp | Depends | Status |
|---|---|---|---|---|
| WP3.0 | Interface headers + dock layout spec | Orch | M2 | ⬜ |
| WP3.1 | `core/clip-index` | A | 3.0 | ⬜ |
| WP3.2 | `platform/thumbnails` (Shell API, async, cache) | A | 3.0 | ⬜ |
| WP3.3 | `platform/shell-ops` (reveal, Recycle Bin, open) | A | 3.0 | ⬜ |
| WP3.4 | `ui/clip-list-model` + list view | B | 3.1, 3.2 | ⬜ |
| WP3.5 | `ui/status-header` + `ui/banner-area` | B | 3.0 | ⬜ |
| WP3.6 | `ui/clip-dock` assembly + right-click menu + registration | C | 3.3–3.5 | ⬜ |

- **WP3.0 (Orch)** writes a layout spec with an ASCII wireframe and the widget tree, so
  the three UI WPs line up. It describes:
  - the header: current game, Replay Buffer pill, auto-sort toggle
  - the banners: Replay Buffer disabled, clips folder missing, legacy script loaded,
    update available
  - the list: game filter, search, rows with a thumbnail, type icon, time and size
  - the footer: Open folder, ⚙
- **WP3.1** scans `<base>` once at startup to build the index, then updates it on each
  `clip_sorted` callback. It supports grouping, filtering and search, and caps the number
  of recent clips held in memory. Unit-tested against fixture directories.
- **WP3.2** uses `IShellItemImageFactory` on a worker thread, with an LRU cache and a
  placeholder image while loading. The stub version returns no image.
- **WP3.3** uses `SHOpenFolderAndSelectItems` to reveal a file in Explorer,
  `IFileOperation` with `FOFX_RECYCLEONDELETE` for deleting to the Recycle Bin, and
  `ShellExecuteW` to open a clip. Rename and "Move to game…" go through `file-mover`.
- **WP3.6** registers the dock with `obs_frontend_add_dock_by_id("clip-atlas-dock", …)`.
  ⚙ opens the settings dialog.

### M4 — Game rules and full settings

| ID | Title | Grp | Depends | Status |
|---|---|---|---|---|
| WP4.0 | Interface headers for M4 | Orch | M3 | ⬜ |
| WP4.1 | `data/games.json` + loader ★ | A | 4.0 | ⬜ |
| WP4.2 | `platform/process-info` (exe ProductName via window → pid → path) | A | 4.0 | ⬜ |
| WP4.3 | `core/scripts-import` (read legacy settings, detect loaded script) | A | 4.0 | ⬜ |
| WP4.4 | `ui/rules-editor` (Games page) | B | 4.1 | ⬜ |
| WP4.5 | Full settings dialog (Detection, Replay Buffer, Advanced pages) | B | 4.3 | ⬜ |
| WP4.6 | Wiring: "legacy script loaded" banner, "add the game running now" | C | 4.2–4.5 | ⬜ |

- **WP4.1 ★: avoid made-up data.**
  - **Only include an exe → name mapping the worker is certain of.** It's better to ship
    fewer entries than wrong ones. Each entry gets a `"confidence": "high"` field.
  - The Reviewer spot-checks at least 20 entries.
  - The schema is `{ "version": 1, "games": [ { "exe": "...", "name": "..." } ] }`.
  - The loader reads it with `obs_data_create_from_json_file`, then converts it into the
    map `name-resolver` expects.
- **WP4.3** parses the scene collection JSON: `modules` → `scripts-tool` → a list of
  `{path, settings}`. It finds `rb_event_driven.py` there and pulls out its
  `base_output_path` and `source_name`. Pure logic, tested against fixture JSON. The
  orchestrator confirms the real structure against an OBS 31 scene collection file before
  writing the brief.

### M5 — Release

| ID | Title | Grp | Depends | Status |
|---|---|---|---|---|
| WP5.1 | `core/version` (semver compare) + `platform/http` (WinHTTP) | A | M4 | ⬜ |
| WP5.2 | Update checker (once per day, banner, setting to turn off) | B | 5.1 | ⬜ |
| WP5.3 | Inno Setup installer + zip, CI release job on tag | A | M4 | ⬜ |
| WP5.4 | Hotkeys (toggle auto-sort) | A | M4 | ⬜ |
| WP5.5 | Docs: README install guide, SmartScreen steps, troubleshooting, CHANGELOG | B | 5.3 | ⬜ |
| WP5.6 | Release candidate: tag `v1.0.0-rc.1` → draft release | Orch | all | ⬜ |

- **WP5.3** installs to `%ProgramData%\obs-studio\plugins\clip-atlas\`, which requires
  admin rights.
  - It detects whether `obs64.exe` is running and asks the user to close it.
  - It registers an uninstaller and keeps the user's settings when uninstalling.
  - The orchestrator confirms that Inno Setup is available on the `windows-2022` runner
    image, or the job installs it.

---

## 6. Risks to watch

| Risk | Mitigation |
|---|---|
| Haiku invents OBS API names or signal fields | Every brief quotes the exact API signatures from the OBS 31 headers. The orchestrator checks them against the source, then CI compile-checks the code. |
| Haiku invents game exe names | WP4.1 confidence rule and Reviewer spot-check |
| Parallel WPs conflict | Strict file ownership, separate worktrees, headers committed before dispatch |
| Glue code can't be tested on Linux | Keep the glue thin. Logic lives in `core/`. CI compiles everything. The user's manual checklists cover the rest. |
| Losing user clips | ★ review on file-mover. Never delete the source without a verified copy. Tests include failure injection. |
| Context loss across sessions | `plan.md` status, `docs/handover/*.md` and the decision log are the source of truth. A new orchestrator resumes from them. |

---

## 7. Decision log

| Date | Decision | By |
|---|---|---|
| 2026-10-20 | Product decisions recorded in `docs/PLAN.md` | User + Claude |
| 2026-10-20 | Folder layout `<base>/<Game>/<YYYY-MM>/`, monthly folders optional and on by default; implemented in `core/clip-layout` | User + Claude |
| 2026-10-20 | Subagent-driven build: Opus orchestrator, Haiku 5.5 workers at max effort | User |
| 2026-10-20 | WP0.1: template HEAD 3e7d7ac with the OBS 31.0.0 / deps 2024-09-12 pin from template commit 5da535c; libobs configured with `-DENABLE_UI=OFF` (31.0's name; template HEAD's `ENABLE_FRONTEND` doesn't exist in 31.0) | Orchestrator |
| 2026-10-20 | CI runs on pushes to `claude/**`, so each WP push is checked before a PR exists; packaging stays on as in the template | Orchestrator |
| 2026-10-20 | Local compile check uses Ubuntu 24.04's own `libobs-dev` 30.0.2 + Qt 6.4.2 (the OBS PPA is blocked by the container proxy). It catches most glue/UI errors; APIs new in 31.0 are only checked by Windows CI. Ubuntu CI builds against whatever the OBS PPA serves | Orchestrator |
| 2026-10-20 | Formatting tools pinned to OBS's: clang-format 19.1.x, gersemi 0.21.0 | Orchestrator |
| 2026-10-20 | `buildspec.json` author `sxnfer`, email `sxnfernandes@outlook.com` (the address already public in this repo's commit history) | Orchestrator |
| 2026-10-20 | Root `CMakeLists.txt` globs `src/{core,obs,ui}/*.cpp` and `src/platform/*-win.cpp` (Windows) or `*-stub.cpp` (elsewhere) with `CONFIGURE_DEPENDS`; work packages add files and never edit it. Known gap: in an existing build tree, a newly added `Q_OBJECT` header needs a clean build (CI always builds clean) | Orchestrator |
| 2026-10-20 | Format check covers the whole repo (`build-aux/check-format`, clang-format 19 + gersemi 0.21) on every push and PR; unit tests run on Ubuntu and Windows in CI. Markdown-only PRs keep the template's `paths-ignore` | Orchestrator |
| 2026-10-20 | WP0.4 contracts: sources are identified by UUID; the clip folder is named by the resolved display name, and the lowercased exe is the key for rules and grouping; OBS's `"unknown"` exe counts as no exe | Orchestrator |
| 2026-10-20 | An ignored game (user rule) is treated as no game: it never becomes current, never starts the Replay Buffer, and its clips go to the unsorted folder | Orchestrator |
| 2026-10-20 | Replay Buffer: "we started it" is recorded only on `REPLAY_BUFFER_STARTED` while our start is pending (OBS's start can silently do nothing). If the user stops the buffer while a game is current, we don't restart it until the current game changes | Orchestrator |
| 2026-10-20 | Replays are remux-aware like recordings: OBS remuxes right after `REPLAY_BUFFER_SAVED`, so the sorter waits for the remuxed file and moves both. New `core/remux-rules`, implemented in WP1.3 | Orchestrator |
| 2026-10-20 | `settings.json` is read and written on the UI thread (small file, via `obs_data_save_json_safe`): an accepted exception to "no file I/O on the UI thread" | Orchestrator |
| 2026-10-20 | file-mover never deletes a source without a same-size copy; if the source can't be deleted after copying, the result is `copied_source_kept` (a duplicate, never a loss) | Orchestrator |
| 2026-10-20 | Auto Replay Buffer reacts only to real game hooks (not show/activate), matching the script closely enough for now | User |
| 2026-10-20 | Optional save sound added as WP2.5: Windows playback, off by default, no device routing, and the Desktop Audio capture caveat is documented rather than worked around | User |
| 2026-10-20 | `sanitize_folder_name` puts the device-name `_` on the stem (`COM1_.exe`), not at the end: Windows reserves `COM1.<anything>`, and the old form also grew by one `_` per save (found by WP1.4) | Orchestrator |
| 2026-10-20 | `run_on_ui()` runs synchronously when called on the UI thread: OBS 31's UI task handler uses `Qt::AutoConnection` (found by WP1.6); the header now says so and UI-thread callers must not rely on deferral | Orchestrator |
| 2026-10-20 | file-mover (★ review): the only copy is never deleted (a partial is kept if the source vanishes mid-copy), the source is re-verified before deletion, any non-absent destination counts as taken, and cleanup/removal have their own 5 s budgets. Deferred: a no-replace publish primitive on Windows (`MoveFileW` without the replace flag via an injected FileOps); the residual race is microseconds and only our worker writes to clip folders | Orchestrator |
| 2026-10-20 | The Replay Buffer controller starts on `OBS_FRONTEND_EVENT_FINISHED_LOADING`: OBS 31 loads modules before `ResetOutputs()`, so `obs_frontend_get_replay_buffer_output()` during module load would dereference a null output handler (found via WP1.9) | Orchestrator |
| 2026-10-20 | Frontend callbacks are never removed, only made inert: OBS dispatches events with an index loop, so a removal during EXIT makes another plugin's callback run twice or be skipped (found by the WP1.7 ★ review). At EXIT the plugin stops everything but destroys nothing; objects are destroyed in `obs_module_unload`, after OBS has dropped all callbacks | Orchestrator |
| 2026-10-20 | M1 accepted after the user's Windows test on OBS 32.2.2 (newer than the 31.0 build target): the plugin works on 32.x | User |
| 2026-10-20 | WP2.0: one filing pipeline, `obs/clip-filer`, for replays, recordings and screenshots (M1's replay code made generic). `FrontendEvents` (replays + screenshots) and `RecordingEvents` each own a `ClipFiler`, so a recording's long remux wait never delays a replay | Orchestrator |
| 2026-10-20 | `ClipKind`, `ClipGame`, the game resolution for a clip and `sorting_enabled` move to `core/clip-routing` (tested); `ClipSorted` moves to `clip-filer.hpp` and gains a `save_id` shared by every file of one save | Orchestrator |
| 2026-10-20 | Recordings: every part of a split recording is moved when the recording stops, into one folder. The first file comes from `obs_frontend_get_last_recording()` at `RECORDING_STARTED` (OBS sets it in `StartRecording()`), the rest from `file_changed`'s `next_file`. Advanced + FFmpeg output to a URL is not a file and is not tracked | Orchestrator |
| 2026-10-20 | Dominant game counts only time with a game current; with none it falls back to the game at start. The option is shown on the General page in M2 so it can be tested; it moves to the Detection page in M4 | Orchestrator |
| 2026-10-20 | Remux wait timeout: 10 min for replays (as in M1), 60 min for recordings | Orchestrator |
| 2026-10-20 | The save sound plays for replays only, once per save: it confirms a replay hotkey press. Recordings and screenshots stay silent (a one-line change in `plays_save_sound` if the user wants them) | Orchestrator |
| 2026-10-20 | Save-sound volume gain is (volume/100)², applied to the samples of an in-memory copy played with `SND_MEMORY`; PCM 8/16/24/32-bit and float32 can be scaled, and at 100 % any WAV plays unchanged. Default volume 80 % | Orchestrator |
