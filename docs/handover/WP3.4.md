# WP3.4 hand-over: `ui/clip-list-model` + row delegate (worker)

## Summary

`ClipListModel` (a `QAbstractListModel` over `ClipIndex`) and `ClipListDelegate`, built to the contract in
`src/ui/clip-list-model.hpp`.

- **Rows**: `index().query(query)`, rebuilt on every change. `reset_clips`, `upsert_clip`, `remove_clip` and
  `set_query` each reset the model, so the dock reselects by path.
- **Thumbnails**: a miss asks `thumbnail_provider()` once per path. Each answer hops to the UI thread through
  `run_on_ui()`, guarded by a `QPointer`. Pictures are kept in an `LruCache` of 300 pixmaps. A failed path is not asked
  again until its file changes or the list is rescanned. Every request carries a ticket, so an answer for a request
  that was forgotten or superseded is ignored (Deviations, item 1).
- **Delegate**: paints each row from the model's roles. The background is the view's own
  (`PE_PanelItemViewItem`), so every OBS theme works. The file name is elided in the middle, the kind sits at the
  right of line 1, and line 2 reads game · age · size in the placeholder colour. All strings come from `Dock.*` keys,
  and all eight used keys exist in `en-US.ini`.
- `refresh_ages()` emits `dataChanged` over every row with no roles. `shutdown()` is idempotent and does not touch the
  provider.

The `public:` section of `clip-list-model.hpp` is byte-identical to HEAD, and the diff touches only the includes and
the private section. Nothing is committed. The two files are staged with `git add -A`.

**One finding needs a decision before WP3.6: the width `sizeHint` returns (Known gaps, item 1).** With the option's
width, the right 14 px of every row are clipped whenever the list has a scrollbar, and the kind label is cut.

## Files changed

| File | Change |
|---|---|
| `src/ui/clip-list-model.cpp` | new: the model and the delegate (458 lines) |
| `src/ui/clip-list-model.hpp` | private section only, plus three `#include` lines (Deviations, item 1) |
| `docs/handover/WP3.4.md` | this note |

## How it was tested

Commands were run in the worktree `agent-a61c85c9cb89e0732`.

1. **Step 0**: `git merge --ff-only claude/pensive-hamilton-cbg1l4` (fast-forward `417ab86..17e3025`), then
   `git log -1 --oneline` → `17e3025 WP5.6: README, install guide and changelog`.
2. **Linux plugin, warnings as errors**: `cmake --preset ubuntu-ci-x86_64` (configured with
   `CMAKE_COMPILE_WARNING_AS_ERROR=TRUE`), then `cmake --build build_x86_64` → exit 0. The same build before my file
   was added also passed (34 steps, no warnings). `clip-list-model.cpp` compiles with no warnings. The link succeeds
   with two undefined symbols, `thumbnail_provider` and `ThumbnailProvider::request`, because the WP3.2 stub is not on
   this branch (Known gaps, item 4).
3. **Unit tests**: `cmake -S tests -B build/tests && cmake --build build/tests && ctest --test-dir build/tests --output-on-failure`
   → 343 of 343 passed. Catch2 was fetched by FetchContent.
4. **Format**: `clang-format-19 --style=file --dry-run --Werror src/ui/clip-list-model.cpp src/ui/clip-list-model.hpp`
   is clean. `git add -A && build-aux/check-format` → `all 83 C/C++ and 16 CMake files are formatted`, exit 0.
5. **Scratch Qt harness** (outside the repo, in the session scratchpad at
   `/tmp/claude-0/-home-user-auto-clip-sorter/6c995450-a095-5948-9d9e-9af008631cf2/scratchpad/wp34-scratch/`; not
   committed). It compiles `clip-list-model.cpp`, `clip-index.cpp` and `clip-layout.cpp` in place, with fakes:
   `obs_module_text` reads `data/locale/en-US.ini`, so the age and kind text is the real English; `obs_log` counts
   warnings; `run_on_ui` runs inline on the UI thread and queues from any other thread; `ThumbnailProvider` records each
   request, and the test answers it on the UI thread or from a worker thread (ok, failed, dropped, or a malformed ok).
   Qt 6.4.2 (system), GCC 13.3, `QT_QPA_PLATFORM=offscreen`.

   | Build | Result |
   |---|---|
   | Plain, `-Wall -Wextra -Wpedantic -Wshadow -Werror` | 184 checks, 0 failed, 0 warnings logged |
   | ASan + UBSan (`-fno-sanitize-recover=undefined`) + LSan | 184 checks, 0 failed, no reports |
   | TSan, view test left out (`--no-view`) | 177 checks, 0 failed, no reports |
   | TSan, full | 184 checks, 0 failed, 18 reports, all inside Qt's own threads (Known gaps, item 6) |

   What the checks cover:
   - **Rows**: reset; upsert (new, moved, filtered out); remove; game, unfiled and search filters (case, spacing,
     several terms); the 5000 cap (an older clip is refused, a newer one pushes out the oldest).
   - **Roles**: display, tooltip, path (non-ASCII included), kind, game, size, modified (round trip through QDateTime),
     an invalid index, an unknown role, and a child index.
   - **Thumbnails**: one request per miss and none while pending; ok → cached and `dataChanged(row, DecorationRole)`;
     failed → no further request; dropped → the next paint asks again; a malformed ok → failed; a filter change keeps
     pictures; a rescan keeps the picture of an unchanged file and drops a changed one; a changed file clears its
     failed mark, and so does a rescan.
   - **Stale answers**: a file replaced while its request is out (ignored); a rescan superseding a request (the old
     dropped answer is ignored, and the newer request stays pending); remove forgets the picture.
   - **Shutdown**: idempotent; cached pictures still show; no new requests; answers after shutdown are ignored.
   - **Model destroyed while a request is out**: an answer on the same thread and an answer queued from a worker thread
     are both ignored, without a crash.
   - **Answer from a worker thread**: not applied until the event loop runs, then applied on the UI thread.
   - **`refresh_ages()`**: one `dataChanged` over all rows with no roles; none for an empty model.
   - **Delegate**: sizeHint height is 64 and its width is the option's. Painting into a `QImage` for selected and
     unselected rows, with and without a picture: the placeholder icon is drawn; the text is drawn with the real `Dock.*`
     strings (the keys are asserted); the selection uses the highlight colour and an unselected background stays
     untouched; the picture is scaled and centred (pixel checks); HiDPI at device pixel ratio 2 (pixel checks); the
     painter's clip, font and the view's clip are restored; narrow, very long and zero-width rows; a `QWidget` supplies
     the style.
   - **QListView integration** (same harness): row height 64; the selection highlight in a real view; a row repaints
     when its picture arrives; what a rescan does to scroll and selection (Findings).

## Findings during testing

- `PE_PanelItemViewItem` paints the full-row highlight only when `option.showDecorationSelected` is set.
  `QAbstractItemView::viewOptions()` sets it from `SH_ItemView_ShowDecorationSelected`, and the delegate copies the
  view's option, so the dock gets it. My first harness run missed the highlight because the harness's option lacked
  the flag. The harness now builds its option the way the view does.
- `QStyledItemDelegate::initStyleOption()` is protected in Qt 6.4. `paint()` calls it from inside the derived class,
  which is legal.
- `ClipListModel::index()` hides `QAbstractItemModel::index(row, column, parent)` (Known gaps, item 3).
- In a `QListView` of 40 rows at 420×200, uniform rows take the width that `sizeHint` returns. With the option's width,
  rows are 418 px wide while the viewport is 404 px once the scrollbar shows. With a width of 0 or -1, rows match the
  viewport (404 px with the scrollbar, 418 px without). `QListView::Adjust` resize mode does not change this.
- A rescan keeps the scroll position (640 → 640) and clears the selection (1 → 0). Visible rows still pending were
  asked again (4 requests).

## Deviations (and why)

1. **Header, private section.** `#include "thumbnails.hpp"` is needed because the private `on_thumbnail(...)`
   declaration takes `ThumbnailStatus` and `Thumbnail`. `<cstdint>` and `<map>` are added for the ticket map. Private
   additions: `on_thumbnail`, `forget_thumbnail`, `next_ticket_`. `pending_` changes from `std::set<std::string>` to
   `std::map<std::string, std::uint64_t>` (path → ticket). The public part is unchanged.
   *Why tickets:* a set of paths cannot tell an answer for the current request from one for a request that was
   forgotten or superseded. Without tickets, a file replaced while its request was out would get the old picture, and
   a `dropped` answer for a request that the provider had replaced would clear the newer request's pending mark. Tickets
   are never reused, so such answers are ignored.
2. **`reset_clips` keeps pictures.** A picture survives a rescan when the file's size and modified time are unchanged,
   so a Refresh does not blank the list. The contract allowed clearing everything; this is the more precise option.
   The old and new clips are matched through a `std::map` keyed by path. Failed marks and pending requests are cleared
   as a whole, so a failed clip gets one retry per scan.
3. **`upsert_clip` also forgets a path that is new to the index.** This costs nothing, and it covers a picture left
   behind by a clip that the 5000 cap pushed out.
4. **A malformed `ok` counts as `failed`.** An empty image, or one whose pixel bytes are fewer than
   width × height × 4, is never copied into a `QImage`.
5. **HiDPI scaling (a refinement).** The picture is scaled in device pixels, using the painter device's device pixel
   ratio, and drawn at logical size. The provider is asked for 192 px, twice the 96 px box, so a 2x screen gets a sharp
   picture. The contract's `KeepAspectRatio` and `SmoothTransformation` are kept.
6. **Logging.** `data()` and `paint()` catch and call `obs_log(LOG_WARNING, ...)`, as CLAUDE.md asks ("catch at the
   edge and log"). This changes no behaviour.

## Known gaps and risks

1. **Rows overflow the viewport when a scrollbar shows (needs a decision).** `sizeHint` returns the option's width, as
   the header says. `QListView` then sizes uniform rows to that width, which is the full view width, not the viewport.
   Measured (Findings): 418 px rows against a 404 px viewport with the scrollbar shown, so the rightmost 14 px are
   clipped. The kind label is right-aligned to the 4 px margin, so it is cut too. A width of 0 or -1 makes `QListView`
   use the viewport width in both cases. I did not change it, because the header specifies the option's width. The
   fix is `QSize(0, ClipListModel::kRowHeight)` in `ClipListDelegate::sizeHint`, plus the matching header sentence.
2. **Every reset clears the selection.** That covers filter keystrokes, upserts, removes and rescans. The scroll
   position survives a rescan (Findings). WP3.6 must reselect by path after each change, which the header already says.
3. **`ClipListModel::index()` hides the base `index(row, column, parent)`.** Code that holds a `ClipListModel *` must
   write `QAbstractListModel::index(row, 0)`. The model itself does this where it needs an index. WP3.6 must know.
4. **Windows and thumbnails.** The Windows build was not compiled here (no MSVC or Windows SDK in this environment). The
   code uses only Qt, the C++ standard library and existing repo helpers. Until WP3.2's stub is on the branch, the Linux
   `.so` has two undefined thumbnail symbols. The MODULE link accepts that, but a Linux load of the module would fail
   until the stub lands. I did not load the module in OBS.
5. **`QPointer` is copied on the provider's thread** (the contract's design). The `QWeakPointer` copy constructor in Qt
   6.4 (`qsharedpointer_impl.h`) reads two of its own members and makes one atomic reference-count increment, and the
   destructor only does an atomic decrement, so a copy does not race with the object's destruction. The tests copy it
   on a worker thread while the model is alive. The case "the model is destroyed while another thread copies the
   pointer" is not exercised.
6. **ThreadSanitizer reports with the view test (18).** Every report is inside Qt's own pooled threads, which Qt starts
   itself while a window is set up. No plugin frame appears in any report. A Qt-only control, with no plugin code, reproduces
   TSan reports too (5 in one run). With the view test left out, the run is clean.
7. **Cache and retries.** Pictures of clips that left the index stay in the LRU until eviction, which is bounded at
   300. Each rescan asks once for each visible failed clip again.
8. **Not measured:** the cost of a row rebuild (it copies up to 5000 entries per change), search keystroke latency, and
   painting with 300 cached pictures.
9. **Not checked visually:** hover and focus rectangles. The delegate draws what the style draws.

## Questions for the orchestrator

1. **`sizeHint` width (Known gaps, item 1):** may I change it to `QSize(0, kRowHeight)`, and update the header's
   "the option's width"? The change is measured to fix the overflow. I did not make it.
2. **Public part:** may `ClipListModel` gain `using QAbstractListModel::index;`, so that WP3.6 can call
   `model.index(row, 0)`? I did not add it, because the public part is fixed.
3. **Private changes:** do you accept the header changes in Deviations, item 1 (the three includes, `pending_` as a
   ticket map, and the new private members)?
4. **Reset behaviour:** do you accept the picture retention on reset in Deviations, item 2?

## Orchestrator integration notes

- Read the whole diff; only the WP's files changed (the header in its includes and private section).
- Questions 1 and 2, done by the orchestrator: `sizeHint` returns width 0 (measured by the worker to fit the
  viewport with and without a scrollbar; the header comment says why), and `using QAbstractListModel::index;`
  brings back the base `index(row, column, parent)`.
- Questions 3 and 4: the ticket map and keeping unchanged files' pictures across a rescan are accepted.
- Gates: plugin build with warnings as errors, no undefined `clip_atlas` symbols left in the Linux module
  (`nm -C ... | grep " U " | grep clip_atlas` is empty now that WP3.2's stub is on the branch), check-format clean.
