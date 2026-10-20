# WP1.2 hand-over: core/game-state (★)

**Status:** done locally. Not committed. Needs the ★ review.

## Summary

- `src/core/game-state.cpp` implements the `GameState` contract in `src/core/game-state.hpp`. The header is unchanged.
- The current game is the hooked source that is live in the Program output. If none is live, it is the most recently hooked source. A source keeps its liveness across an unhook. `source_removed` and `clear` forget it.
- Whenever the current game changes or goes away, the old one becomes the last-seen game, with its timestamp. `game_for_save` returns it up to `grace_period()` after that, inclusive. `no_game_since` gives the start of the gap, which WP1.3 uses to time the Replay Buffer stop.
- `tests/test-game-state.cpp` has 25 Catch2 cases. They use fixed time points only. Every acceptance item in the brief maps to at least one case (table below).
- Checked by mutation: 16 deliberate faults in the `.cpp`, each caught by at least one test.

## Files changed

- Added `src/core/game-state.cpp` (139 lines).
- Added `tests/test-game-state.cpp` (390 lines, 25 test cases).
- Added `docs/handover/WP1.2.md` (this note).

Nothing else changed. `src/core/game-state.hpp` is untouched, so there are no private-section additions. `tests/CMakeLists.txt` globs pick up the new files without edits. `build/` and `build_x86_64/` were created by the builds and are gitignored.

For the commit, stage these three files: `src/core/game-state.cpp`, `tests/test-game-state.cpp`, `docs/handover/WP1.2.md`.

## How it was tested

Base: `git merge --ff-only claude/m1-feature-parity` fast-forwarded `77f7bcd..94b3fd0`. `git log --oneline -1` shows `94b3fd0 Plan: add optional save sound (WP2.5)`, and `src/core/game-state.hpp` exists.

| Check | Command (from the worktree) | Result |
|---|---|---|
| Baseline, before the change | `cmake -S tests -B build/tests -G Ninja && cmake --build build/tests -j4 && ctest --test-dir build/tests --output-on-failure` | 11 of 11 pass (Catch2 v3.7.1 fetched) |
| Acceptance | Same command, after the change | `100% tests passed, 0 tests failed out of 36` (11 existing, 25 new) |
| Test-build warnings | Forced rebuild of the two new units with the tests' flags (`-Wall -Wextra -Wpedantic`), GCC 13.3 | 0 warnings |
| CI preset | `cmake --preset ubuntu-ci-x86_64 && cmake --build --preset ubuntu-ci-x86_64` | Both succeed. `game-state.cpp.o` is compiled with `-Werror`, and the log has 0 warnings. CMake notes that `ENABLE_CCACHE` is unused. That comes from the template preset, and ccache is not installed here. |
| Format | `clang-format-19 --dry-run --Werror src/core/game-state.cpp tests/test-game-state.cpp` | Exit 0 (clang-format 19.1.1). One expression in `game-state.cpp` was reflowed by `clang-format-19 -i`. |
| Strict GCC | `g++ -std=c++17 -fsyntax-only -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wsign-compare -Wold-style-cast -Wcast-qual -Wunused -Werror` on both files. Catch2 headers are passed with `-isystem`. | Exit 0 (GCC 13.3) |
| Strict Clang | `clang++ -std=c++17 -fsyntax-only -Weverything -Werror` on both files, with `-Wno-c++98-compat`, `-Wno-c++98-compat-pedantic`, `-Wno-padded`, `-Wno-missing-prototypes`, `-Wno-exit-time-destructors` and `-Wno-global-constructors` | Exit 0 (Clang 18.1.3) |
| Mutation | 16 faults, applied one at a time to a copy of `game-state.cpp` with perl substitutions. Each run rebuilt the tests and ran `ctest`. The original was restored and checked with `cmp`. | Every fault makes at least one test fail (table below). |

Mutation results (tests failing out of 36):

| Fault | Tests failing |
|---|---|
| Grace boundary made exclusive (`>=`) | 5 |
| No ceil, exact nanosecond compare | 1 (the very-large-grace test) |
| No backwards-clock guard in `game_for_save` | 1 |
| Liveness ignored when choosing the winner | 4 |
| Oldest hook wins | 12 |
| Repeat hook keeps its old order | 3 |
| Unhook drops liveness | 1 |
| `source_removed` keeps the entry | 2 |
| Last-seen game never recorded | 8 |
| Last-seen time never recorded | 7 |
| `no_game_since` never cleared | 3 |
| Info ignored in the change check | 1 |
| Negative grace not clamped | 1 |
| `game_for_save` ignores the current game | 1 |
| `clear()` reports no change | 2 |
| `source_live()` reports no change | 2 |

Brief item to test case:

| Brief item | Test case |
|---|---|
| Single source hook/unhook; `current()` and `no_game_since()` | a single hooked source is the current game until it unhooks |
| Two hooked sources, most recent hook wins | the most recently hooked source wins when both are hooked |
| Live beats a more recent non-live source | a live source beats a more recently hooked one that is not live |
| Live toggling flips the winner | toggling liveness flips the winner between two hooked sources |
| Repeat hook on the older source | re-hooking the older source makes it the most recent |
| Liveness set before hooking | liveness reported before a hook is remembered |
| Source removed while hooked | removing the current source falls back to the other hooked source; removing a source that is not current leaves the game alone |
| `clear()` with a current game | clear() makes the cleared game the last seen one |
| Grace edges: exactly grace, grace + 1 ms, grace 0, negative clamped | the last seen game is returned up to and including the grace period; the grace boundary is exact to the nanosecond; a zero grace period covers only the instant the game went away; a negative grace period is clamped to zero |
| `game_for_save` while current ignores last-seen | game_for_save returns the current game and ignores the last seen one |
| Mutator return values, including info-only re-hook | mutators return true only when current() changed |
| Unknown ids for `unhooked` and `source_removed` | unhooked and source_removed ignore unknown ids |

Extra cases beyond the brief: among live sources the most recent hook wins; liveness survives an unhook; removing a source forgets its liveness; unhooking a source that is not current leaves the game alone; `set_grace_period`; a very large grace period; `game_for_save` before the game ended; the last seen game is the most recently ended one; `no_game_since` across game switches.

MSVC is not installed here. The MSVC /W3 requirement is covered by the strict GCC and Clang runs and by review, not by an MSVC build.

## Deviations from the contract

None in behaviour. These are implementation choices that callers cannot see:

1. **Rounded grace check.** `game_for_save` compares `std::chrono::ceil<std::chrono::milliseconds>(now - last_seen_at_)` with the grace period, not the raw nanosecond difference. The grace period is whole milliseconds, so this gives exactly `elapsed <= grace`. It also avoids converting a huge grace (such as `milliseconds::max()`) to nanoseconds, which overflows. The naive version fails only the very-large-grace test.
2. **No-op early return.** `unhooked()` returns `false` for an unknown or already-unhooked id without calling `update_current()`. Nothing changed, so the result is the same.
3. **File-local `same_game()`.** `CurrentGame` has no `operator==`. Adding one would change the public header, which is off limits.
4. **Parameter name.** The `.cpp` names the grace parameter `grace`, not `grace_period`, so it does not shadow the `grace_period()` member function. Parameter names are not part of the API.
5. **Private section.** No additions. Only the members the header already declares are used.

## Known gaps and risks

1. **Repeat hooks move a source to the front.** The contract gives each `hooked()` call a new order, even with identical info. So a source that is hooked again beats any non-live source hooked earlier. I did not check how often OBS emits `hooked`. The WP0.4 note says it fires from `video_tick`. If it fires every frame, WP1.7 must forward only real transitions: a new hook, or a changed `HookInfo`.
2. **Entries stay until removed.** An unhooked, non-live source keeps its entry until `source_removed()` or `clear()`. `source_live(id, false, ...)` on an unknown id also creates one. WP1.7 must call `source_removed()` when a source is destroyed.
3. **Not thread-safe.** The header documents this. OBS source signals arrive on other threads, so callers must hop to the UI thread (`run_on_ui`) first.
4. **Exceptions.** `std::bad_alloc` can still come out of `std::map` and string copies, as with any standard container. Callers must catch at the OBS boundary, per CLAUDE.md.
5. **Old last-seen info on an info-only change.** A re-hook with a new window sets `last_seen_` to the same source's previous info, because the update rule says so. `last_seen_` is read only while no game is current, and the next transition overwrites it, so no caller can observe the old value.
6. **Platform coverage.** No MSVC here. The CI preset build uses libobs 30.0.2 dev headers, because that is what the container has. The plan targets OBS 31. This unit does not touch OBS, so the difference should not matter, but the check is not an OBS 31 build.

## Questions for the orchestrator

1. Is the repeat-hook rule as intended? A `hooked()` with unchanged info moves the source to the front, and the contract's repeat-hook behaviour requires that. If OBS re-sends `hooked` every frame, should WP1.7 dedupe, or should the rule change?
2. Should `unhooked()` drop a non-live entry to bound memory? The contract says keep it, so I did. Tell me if you want it changed.

## Fix round 1

Changes requested by the coordinator after the first review. The sections above describe the state before this round. Where they differ, this section is current: 43 tests in total, 32 of them new.

### Coordinator answers

- **Repeat hooks.** The repeat-hook rule stays. OBS 31.0 emits `hooked` only on the unhooked-to-hooked transition, never per frame. This closes question 1 and known gap 1 above.
- **Entries.** Kept as specified. This closes question 2.

### What changed

- **Code: `hooked()`** in `src/core/game-state.cpp`. The info is copied into a local before the entry is touched, then moved into the entry. If the copy throws, the entry keeps its old state, so it can never be marked hooked with stale info. Nothing else in the file changed. `src/core/game-state.hpp` is not edited. The orchestrator is rewording the `source_removed` comment in that header.
- **Tests** in `tests/test-game-state.cpp`: seven cases, adapted from the reviewer's draft (its tests 1, 2, 3 and 5) and written for the rest.
  - Blocking: *two sources with identical window info are told apart by source id*. Both sources have the same `HookInfo`. The second hook still changes the current game, so `hooked()` returns true. Removing it falls back to the first source, and removing that one ends the game.
  - Should-fix: *clear() forgets liveness, so a later hook is not live*.
  - Nit: *a re-hook that changes only the executable or the window class is a change*. Each field is changed on its own, and the return value and `current()` are checked after each change.
  - Nit: *HookInfo equality and inequality compare title, window class and executable*. Exercises `operator==` and `operator!=` directly, one field at a time.
  - Grace origin after a fallback: *grace after a fallback runs from when the last game went away*.
  - Removed last-seen: *a game that already went away stays last seen after its source is removed*.
  - Extra: *source_removed while current makes that game the last seen one*. This is the removal path that makes a game last seen, and no earlier test checked it.
- **Test helpers.** `window()` takes an optional window-class argument. `at_ms()` was added for millisecond times.

### Results

Before the fix, all four faults named by the coordinator survived the 36-test suite. Each mutant run ended with `100% tests passed, 0 tests failed out of 36`.

After the fix, each of those faults fails the test it should:

| Fault (applied to a copy of `game-state.cpp`) | Failing test on the 43-test suite |
|---|---|
| N1: `same_game` compares info only, ignoring the source id | two sources with identical window info are told apart by source id |
| N2: `clear()` keeps liveness | clear() forgets liveness, so a later hook is not live |
| N3: `same_game` compares title only | a re-hook that changes only the executable or the window class is a change |
| N4: `same_game` ignores the window class | a re-hook that changes only the executable or the window class is a change |

All 20 faults now fail at least one test. M01 to M16 are the faults in the first round's table, in the same order. Tests failing per fault, out of 43: M01 5, M02 1, M03 1, M04 4, M05 16, M06 3, M07 1, M08 5, M09 11, M10 8, M11 3, M12 2, M13 1, M14 1, M15 2, M16 2, N1 1, N2 1, N3 1, N4 1.

| Check | Command (from the worktree) | Result |
|---|---|---|
| Unit suite | `cmake -S tests -B build/tests -G Ninja && cmake --build build/tests -j4 && ctest --test-dir build/tests --output-on-failure` | `100% tests passed, 0 tests failed out of 43`. The test build log has 0 warnings. |
| CI preset, warnings as errors | `cmake --build --preset ubuntu-ci-x86_64` | Exit 0. `game-state.cpp` is recompiled with `-Werror`, and the log has 0 warnings. |
| Format | `clang-format-19 --dry-run --Werror src/core/game-state.cpp tests/test-game-state.cpp` | Exit 0. Two trailing comments in the test file were realigned by `clang-format-19 -i`. The diff was whitespace only. |
| Strict GCC and Clang | The two strict compiles from the first round, on both files | Both exit 0. |
| Mutation | 20 faults, one at a time. Each was followed by a rebuild and `ctest`. The original was restored and checked with `cmp`, then rebuilt. | Every fault fails at least one test (above). |

### Known gaps (updated)

- `hooked()` is safe if its copy throws. The entry lookup, `sources_[source_id]`, also leaves the map unchanged if it throws, because the entry is changed only after the lookup succeeds.
- `update_current()` is still not exception-safe. If it throws (bad_alloc while copying the winner or `last_seen_`), `sources_` can already show the mutation while `current_` lags. The next mutator call recomputes the winner and corrects it. A full guarantee would need the next state built before any mutation, which was not requested in this round. No test can reach these paths, because the suite has no allocation injection, so this part is checked by reading only.

### Files in this round

- `src/core/game-state.cpp` (`hooked()` only).
- `tests/test-game-state.cpp` (seven cases, helper changes, one formatting pass).
- `docs/handover/WP1.2.md` (this section).

Not committed. `git status --short` shows the same three untracked files as before.

## Orchestrator integration notes

- ★ review: a Reviewer agent found the state machine correct (differential fuzz against a model written from the brief: about 26M checks, 0 mismatches) but the tests too weak (identical HookInfo on two sources, clear() keeping liveness, executable- or class-only changes). Fix round 1 added those tests plus an exception-safe `hooked()`, and each planted fault now fails a test.
- The reviewer's header-wording finding was resolved by the orchestrator: `game-state.hpp` now states that `source_removed` keeps a game that already went away as last-seen (commit 548dc1d).
- For WP1.7: call `clear()` on `SCENE_COLLECTION_CHANGING`; after re-tracking a source, re-sync its state with the `get_hooked` proc; forward only real hook transitions.
- Gates re-run in the main checkout: check-format clean, 131/131 tests, Ubuntu CI-preset plugin build OK.
