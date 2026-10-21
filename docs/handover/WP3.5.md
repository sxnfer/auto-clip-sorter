# WP3.5 hand-over: `ui/status-header` and `ui/banner-area` (worker)

## Summary

The two widgets at the top of the M3 dock, built to the contracts in `src/ui/status-header.hpp`
and `src/ui/banner-area.hpp`.

- **StatusHeader**: the game label (elided at the right, full name as the tooltip, `Dock.NoGame`
  in the palette's disabled text colour when empty), the Replay Buffer pill (rich text whose dot
  colour comes from the palette), and the auto-sort checkbox. The callback runs on user clicks
  only.
- **BannerArea**: a stack of notice rows in `BannerId` order, hidden when empty. A dismissed
  banner stays dismissed until its text changes.

The public part of both headers is unchanged. Only the private sections changed. No Q_OBJECT,
no new dependency, no edit to `en-US.ini` or `CMakeLists.txt`. Nothing committed; the changes
are staged with `git add -A`.

## Files changed

| File | Change |
|---|---|
| `src/ui/status-header.cpp` | new |
| `src/ui/banner-area.cpp` | new |
| `src/ui/status-header.hpp` | private section only: `apply_game_font()` declared |
| `src/ui/banner-area.hpp` | private section only: `Banner` defined here (it was forward-declared and held by pointer); `make_row`, `refresh_row`, `retire_row`, `index_after`, `run_action`, `dismiss`, `update_visibility` added; `rebuild()` removed |
| `docs/handover/WP3.5.md` | this note |

## How it was tested

Commands run in the worktree unless noted.

1. Step 0: `git merge --ff-only claude/pensive-hamilton-cbg1l4`, then `git log -1 --oneline`
   shows `11b9311 WP3.0: format CMakeLists.txt`.
2. Linux plugin with warnings as errors: `cmake --preset ubuntu-ci-x86_64`, then
   `cmake --build build_x86_64`. It passes, both before and after my files (the preset sets
   `CMAKE_COMPILE_WARNING_AS_ERROR=TRUE`). The "ENABLE_CCACHE not used" warning is only because
   ccache isn't installed here.
3. Scratch harness, outside the repo. It lives in the session scratchpad at
   `/tmp/claude-0/-home-user-auto-clip-sorter/6c995450-a095-5948-9d9e-9af008631cf2/scratchpad/wp35-scratch/`
   (`main.cpp`, `CMakeLists.txt`, `fake-include/obs-module.h`). It compiles the two `.cpp` files
   in place, with a fake `obs-module.h`, and a fake `obs_module_text()` that returns the key. Each
   displayed string therefore shows which key was used. Built with
   `-Wall -Wextra -Wpedantic -Wunreachable-code -Werror`, run with `QT_QPA_PLATFORM=offscreen`:
   - **127 checks, 0 failed.**
   - The same code built with ASan, UBSan and LeakSanitizer: **127 checks, 0 failed**, no reports.
   - Header checks: no-game text and colour; dot colour and key for each of the four replay
     states; locale text is HTML-escaped (`<i>On & sure</i>`); a game name containing markup
     shows literally; elision and tooltip when narrow, wide, and narrowing again; the minimum
     width does not depend on the name; a palette change updates both the game label and the
     pill dot; the game font is bold at 1.1x. Checkbox: `set_auto_sort()` never calls back, a
     click calls back with the new state, a callback can replace itself, and no callback is safe.
   - Banner checks: hidden when empty; rows in `BannerId` order whatever the call order; an
     identical re-show keeps the row and runs the new action; a changed text rebuilds the row in
     place; dismiss hides the row at once and deletes it later; dismissed text stays dismissed
     until it changes; `hide_banner` on a dismissed or hidden banner; an action that hides or
     replaces its own banner; destruction with rows still pending deletion; icons are 16 px and
     match the standard warning or information icon; text is shown as plain text.
4. Mutation checks, on copies in the scratchpad (the repo was not touched), to confirm the checks
   can fail:
   - m1, drop `QSizePolicy::Ignored`: caught by the narrowing check. The first version of the
     checks missed this, so I added the shrink step.
   - m4, rows always appended: caught (many failures).
   - m5, dismissed text ignored on show: caught.
   - m6, synchronous `delete` of a dismissed row: caught (the row is gone before the check sees
     it hidden).
   - m2, `toggled` instead of `clicked`: not caught, as expected. `set_auto_sort()` also blocks
     the box's signals with `QSignalBlocker`, so the guarantee holds either way.
5. Core unit tests: `cmake -S tests -B build/tests && cmake --build build/tests &&
   ctest --test-dir build/tests --output-on-failure`. Result: **246 of 246 passed** (Catch2
   v3.7.1 fetched through the proxy).
6. Formatting: `clang-format-19 --style=file --fallback-style=none --dry-run --Werror` on the four
   owned files is clean. Then `git add -A && build-aux/check-format` reports "all 66 C/C++ and 16
   CMake files are formatted." (clang-format 19.x and gersemi 0.21.0, the pinned versions.)
7. Locale: every `Dock.*` key the two `.cpp` files use (9 keys) exists in
   `data/locale/en-US.ini`. None is missing, and no key was substituted.

## Deviations from the contract

1. **Icon pixmap**: `icon.pixmap(QSize(16, 16), devicePixelRatioF())` instead of
   `pixmap(16, 16)`. The logical size is still 16 px; the pixmap is just sharp on HiDPI screens.
2. **FontChange handled in `StatusHeader::changeEvent`.** The game label has its own font, so it
   doesn't follow a font change. Its font is re-derived and the name re-elided on FontChange. The
   protected comment in `status-header.hpp` lists only PaletteChange and StyleChange. I did not
   edit it, because it's outside my section.
3. **The pill word-wraps** (`setWordWrap(true)`), so a narrow dock wraps the pill instead of being
   forced wider. This is not in the contract.
4. **Game label size policy**: horizontal `QSizePolicy::Ignored`. Without it the label asks for
   the full name's width, so the dock can't narrow and the elision never happens (mutation m1).
5. **Rows are rebuilt only when their look changes**: text, kind, action text (and so whether a
   button exists), or dismissible. An identical `show_banner()` only stores the new action and
   keeps the row, so keyboard focus survives and a banner isn't rebuilt on every replay-status
   change. The contract allowed rebuilding; this is an extra.
6. **Palette reset** uses `setPalette(QPalette())`, which makes the label inherit the palette
   again. `QWidget::unsetPalette()` isn't public in Qt 6.4. Checked at runtime:
   `Qt::WA_SetPalette` is clear for a real game name.
7. **`hide_banner()` on a dismissed banner** drops the stored parameters, while the header comment
   says "does nothing if it isn't shown". Nothing observable changes: `is_shown()` stays false, and
   the dismissed text is kept, so a later `show_banner()` behaves the same.
8. **✕ button** also gets `setAccessibleName(Dock.Banner.Dismiss)`, because the glyph alone has
   no name for a screen reader.
9. **`set_auto_sort()`** blocks the box's signals with `QSignalBlocker`, as well as connecting
   `clicked` (the contract allowed either).

## Known gaps and risks

- **Banner icons don't follow a runtime style change.** `BannerArea` has no `changeEvent`
  override (the header's non-private part is fixed), so icons are fetched when a row is built. A
  style change while a banner is showing keeps the old icon until that banner's row is rebuilt.
- **Not checked on screen, and not built on Windows.** The Linux build used Qt 6.4.2 and libobs 30
  headers, and the harness runs offscreen, so it checks logic and geometry only. Nothing has been
  seen rendered: elided text, pill colour contrast, icon alignment, and the real OBS themes are
  all unverified.
- **Real translations aren't exercised.** The harness fakes `obs_module_text()`. The keys are
  checked against `en-US.ini` by grep.
- **No try/catch in these widgets.** An allocation failure (`std::bad_alloc`) propagates to the
  caller. ClipDock (WP3.6) must catch at its OBS-event edges, as CLAUDE.md requires.
- **Threads**: UI thread only, as the contract says. Nothing here is safe to call from the
  graphics or worker threads.
- **Focus** inside a row is lost when that row's look changes. That's acceptable for a notice.

## Notes for WP3.6

- A banner's action runs on the UI thread and may call `show_banner()` or `hide_banner()` for any
  id, including its own. Both are handled.
- `set_game("")` shows `Dock.NoGame`. `set_auto_sort()` never calls the callback; the callback
  runs only for user clicks.
- Repeated `show_banner()` calls with the same look are cheap, so pass the same parameters on
  each scan or status change rather than hiding and showing the banner.

## Questions for the orchestrator

1. Is the FontChange handling (deviation 2) OK? If so, please add FontChange to the `changeEvent`
   comment in `status-header.hpp`, in the protected section, which is outside my ownership.
2. Should `BannerArea` get a protected `changeEvent`, so icons refresh on a style change? That
   changes the header's non-private part, so I've left it out unless you say otherwise.
3. Is the pill word-wrap (deviation 3) acceptable?

## Orchestrator integration notes

- Read the whole diff; only the WP's files changed (both headers in their private sections only).
- Answers: the FontChange handling, the pill's word-wrap and the extra `QSignalBlocker` are accepted;
  `changeEvent` in `status-header.hpp` now mentions FontChange. `BannerArea` gets no `changeEvent`: a
  style change while a banner shows is rare and the icon refreshes with the next update of that banner.
- Gates: plugin build with warnings as errors (both files compiled), 246/246 unit tests, check-format clean.
- Not seen on screen: the widgets are on the M3 manual checklist.
