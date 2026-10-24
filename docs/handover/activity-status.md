# Native activity status

The dock now has a reusable Qt activity monitor directly below `StatusHeader`. Its compact pill opens a floating,
scrollable details panel without moving the clip browser. Replay Buffer state, Auto-sort, banners and browser actions
remain separate.

## Reference and implementation

Reference: [Arc Agent Status](https://uiarc.dev/components/agent-status/markdown), its public component page and
[license](https://uiarc.dev/license). Agent Status is marked **Pro**, rather than MIT. No Pro source, scripts, styles,
logos or assets were copied or incorporated. This is an independent C++17 / Qt Widgets implementation based on the
publicly documented appearance and behaviour. The public page's initial Ready state and documentation were inspected;
a running interactive reference browser was unavailable, so no pixel-perfect comparison is claimed.

The pill has a custom-painted atlas mark, live arc, completion check or failure cross, rolling label, subtle working
shimmer, fixed-cell elapsed counter, hover feedback and focus outline. Label changes take 180 ms; width changes take
240 ms; the popup opens in 260 ms and closes in 180 ms. The panel fades its content in 140 ms. These use Qt cubic easing,
not Arc's physics spring. Success settles to Ready after four seconds; the panel keeps the result. Failures persist.
Long labels elide in the pill and remain available through its accessible name, tooltip and wrapped panel content.

All production colours come from the host palette and all fonts from Qt. The popup explicitly inherits its owner's
palette and font. It is capped at 384 px or the dock width, fits the current screen, and scrolls for split recordings
and history. Resizing during expansion retargets its geometry. Timers stop while hidden or stopped; continuous painting
is limited to live working indicators. Windows' `SPI_GETCLIENTAREAANIMATION` preference disables decorative motion;
hosts can also call `set_reduced_motion()`. The OS preference is sampled at creation and when shown. Unsupported
platforms have no detected system preference.

## Data and threading

`core/activity-model` contains only standard C++17. An accepted event declares every expected original and remuxed
file, using the existing session-wide `save_id`. Subsequent events address a stable file index. This groups remuxed
replays and all split recording parts without counting the same outcome twice. Duplicate, stale, regressive and
unknown updates are ignored. Twelve finished saves are retained by completion time; active saves are never evicted.
Moving/queued saves take precedence over waiting saves, then the most recently finished save is shown. Other saves
are inspectable in the panel.

`ClipFiler` emits observations on the UI thread through the existing `run_on_ui()` / weak shared-state boundary.
`FrontendEvents` and `RecordingEvents` forward their observers to the plugin, which checks `g_stopped` and its existing
`QPointer<ClipDock>` before updating the widget. Observer exceptions are contained. `ClipSorted`, indexing and save-sound
delivery remain intact. Filer shutdown suppresses callbacks; dock shutdown freezes unfinished model records as
cancelled and stops the component. No worker captures the activity widget.

The only `FileMover` change is an optional start observer, invoked on its worker immediately before a dequeued move.
It distinguishes queued work from a running move; its exceptions cannot interrupt filing. Queued cancellations never
announce a start. Rename, verified copy, retries, publication, source removal and shutdown algorithms are unchanged.

Progress means **files actually sorted out of expected files**, never bytes, speed or estimated percentage. The working
indicator is indeterminate. A save finishes only when every expected file has a terminal outcome. A stability timeout
does not become “File ready”: it carries an explicit fallback notice. An unavailable remux is marked separately, and a
verified copy whose source was kept carries a notice. A successfully sorted original can therefore complete with
“review details” while its unavailable remux remains visibly unsorted. Backend error details are shown as plain text
inside a localised wrapper, following the existing dock's error convention. No file I/O was added to the UI thread.

The implementation intentionally offers inspection only. It adds no Pause, Stop, Retry, deletion or file-management
controls.

## Keyboard and accessibility

- Tab reaches the pill and history controls. Enter or Space toggles the pill.
- Escape works from the popup header and its descendant controls; explicit closure restores focus to the pill.
- `Qt::Popup` supplies outside-press dismissal. Its hide event synchronises accessible state.
- A custom Qt accessibility interface exposes a button action and expandable/expanded/collapsed states.
- Names change with activity, and failed names include backend error details. Elapsed time is exposed as the accessible
  value without issuing timer announcements. Step text and symbols carry meaning without colour.
- The custom-painted progress surface retains `QProgressBar`'s accessible range/value interface.

Native screen-reader announcement behaviour and popup mouse/keyboard grabs need verification on Windows. Offscreen
Qt cannot exercise operating-system grabs, and its diagnostics explicitly report that limitation.

## Verification

The default core suite stays independent of Qt and OBS:

```sh
cmake -S tests -B build/tests
cmake --build build/tests
ctest --test-dir build/tests --output-on-failure
```

Optional Qt tests need Qt 6 Widgets + Test and the OBS development headers. Host-boundary stubs replace localisation,
settings lookup and UI dispatch only; the actual activity widget, model, `ClipFiler` and filesystem `FileMover` run:

```sh
cmake -S tests -B build/tests -DCLIP_ATLAS_UI_TESTS=ON
cmake --build build/tests
ctest --test-dir build/tests --output-on-failure
# Optional snapshots, generated by the widget tests:
QT_QPA_PLATFORM=offscreen CLIP_ATLAS_CAPTURE_DIR="$PWD/docs/activity-preview" \
  build/tests/ui/activity_ui_tests '[activity-ui]'
```

Verified in this task:

- **353/353 CTest entries passed:** 352 core tests and one executable containing nine native UI/filing tests.
- Successful sorting, delayed remux output, original/remux grouping, split files, overlapping saves, real filesystem
  failures, duplicate/stale updates, bounded history, cancellation, shutdown, throwing observers and delivery after
  widget destruction.
- Enter/Space, repeated toggles, Escape from history controls, focus restoration, accessible expansion state, popup
  hide synchronisation, narrow resizing during animation, and light/dark native painting.
- Plugin compiled and linked on Linux with **Qt 6.8.2 and OBS 30.2.3 development headers**, using non-Windows platform
  stubs. This is a compatibility compile check, not the supported Windows OBS 31+ runtime.
- Changed C++ files passed clang-format **19.1.7** `--dry-run --Werror`; CMake files passed gersemi **0.21.0**;
  `git diff --check` passed.

Real 10/60-minute remux timeout runs were not waited out; the reducer tests exercise the emitted timeout outcomes.
The complete plugin was not launched in OBS. Windows OBS themes, DPI/scaling, dock movement between screens, live
Windows motion settings, screen readers and actual outside-click grabs still require manual validation.

## Remaining visual differences

- Qt cubic geometry transitions approximate the reference springs. Labels elide instead of using a right-edge mask.
- The completion glyph changes during the ring closure rather than tracing Arc's precise logo morph.
- Updated panel contents fade together; individual step rows do not slide independently, and progress fills discretely
  from actual completed-file counts.
- The panel shows grouped files and a bounded save history rather than Arc's five most recent agent tool calls.
- Colours, font and the atlas glyph follow Clip Atlas/OBS. Preview screenshots use test palettes, not an OBS theme.

## Changed files

- Model: `src/core/activity-model.{hpp,cpp}`.
- Worker observation: `src/core/file-mover.{hpp,cpp}`.
- Native view: `src/ui/activity-status.{hpp,cpp}`; integration: `src/ui/clip-dock.{hpp,cpp}`.
- Motion preference: `src/platform/motion-preference.hpp`, `motion-preference-win.cpp`, `motion-preference-stub.cpp`.
- Filing events: `src/obs/clip-filer.{hpp,cpp}`, `frontend-events.{hpp,cpp}`, `recording-events.{hpp,cpp}`.
- Plugin dispatch: `src/plugin-main.cpp`; localisation: `data/locale/en-US.ini`.
- Tests: `tests/test-activity-model.cpp`, `tests/test-file-mover.cpp`, `tests/CMakeLists.txt`,
  `tests/ui/{CMakeLists.txt,host-stubs.hpp,host-stubs.cpp,test-activity-status.cpp,test-clip-filer.cpp}`.
- Documentation: this note and `docs/activity-preview/` native screenshots with their README.
