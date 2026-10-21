# WP1.4 core/settings-model: hand-over

## Summary

Implemented the behaviour behind the unchanged contract in `src/core/settings-model.hpp`:
`Settings::operator==`, `validated()`, `load_settings()`, `save_settings()`, and the
`migrate_settings()` hook (a documented no-op for schema 1). Tests use an in-memory fake that
implements both `SettingsReader` and `SettingsWriter`. The unit suite passes (34/34, of which
23 are new) and the `ubuntu-ci-x86_64` plugin build compiles with warnings as errors.

One contract requirement is not fully met. `sanitize_folder_name()` in `clip-layout.cpp`
is not idempotent for device names followed by an extension (`COM1.exe`, `CON.txt`,
`nul.x`), so `validated()` is not idempotent for those unsorted-folder names. The fix
belongs in `clip-layout`, which this WP does not own. See the deviations and questions below.

## Files changed

- `src/core/settings-model.cpp` (new): implementation.
- `tests/test-settings-model.cpp` (new): 23 Catch2 test cases.
- `docs/handover/WP1.4.md` (new): this note.

No header, CMake file or other tracked file was changed. The worktree was fast-forwarded
from `77f7bcd` to `94b3fd0`. Nothing is committed.

## How it was tested

Run from the worktree root.

1. `git merge --ff-only claude/m1-feature-parity`: fast-forwarded. `git log --oneline -1`
   shows `94b3fd0 Plan: add optional save sound (WP2.5)`, and `src/core/settings-model.hpp`
   exists.
2. `cmake -S tests -B build/tests && cmake --build build/tests -j4 && ctest --test-dir build/tests --output-on-failure`:
   **100% passed, 34 of 34** (23 settings-model, 11 existing clip-layout). The binary reports
   158 assertions.
3. `cmake --preset ubuntu-ci-x86_64 && cmake --build --preset ubuntu-ci-x86_64`: built
   `clip-atlas.so` with `CMAKE_COMPILE_WARNING_AS_ERROR=ON`, which compiles both `core/`
   files. The configure step prints an "ENABLE_CCACHE not used" warning from the preset; it
   is not from this change.
4. `clang-format-19 --dry-run --Werror src/core/settings-model.cpp tests/test-settings-model.cpp`:
   clean (clang-format 19.1.1).
5. Stricter warnings, since MSVC is not available here: g++ 13.3 and clang++ with
   `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Werror` over both files:
   clean.
6. Test-strength check, in scratch copies outside the repo: 26 deliberate defects, one at a
   time, each run against the unchanged test file. **25 were caught** by at least one test
   (wrong clamp, no trim, no dedupe, wrong key, unvalidated load, mis-set default, throw on
   newer schema, and so on). The one survivor is "migration hook skipped", which is expected
   because the hook is a no-op (see gaps).
7. Idempotence probe, in scratch: `validated()` applied four times gives
   `COM1.exe` -> `COM1.exe_` -> `COM1.exe__` -> `COM1.exe___` -> ... (same for `CON.txt`,
   `nul.x`). `CON` and `a:b` settle immediately.

## Deviations from the contract, and why

- **`validated()` is not idempotent for "device name + extension" unsorted folders.** The
  contract says to call `sanitize_folder_name(value, "Unsorted")`, and I did. But
  `src/core/clip-layout.cpp` lines 114-115 append `_` to the end of the whole name, so each
  pass adds another underscore. Every load/save therefore grows such a folder name by one.
  The fix is in `clip-layout`, which is outside my ownership, so I did not work around it.
  The idempotence test covers the other inputs, and a comment in it explains the exclusion.
  I did not add a failing test.
- **`schema_version` is also clamped to the int range on read.** It is an int field, so the
  contract's "clamp before narrowing" rule applies. `migrate_settings()` takes an `int`, so
  a huge stored version never triggers a spurious migration.

## Known gaps and risks

- The idempotence gap above is user-visible. A user who types `CON.txt` as the unsorted
  folder sees the name grow on each settings load and save. It is unlikely, but it is real.
- The migration hook is a no-op. The "older schema version" test shows no error and no
  change, but it cannot show that the hook ran. It needs a real test when schema 2 lands.
- Not compiled with MSVC here. `/W3` (plugin) and `/W4` (tests) compliance is reasoned, not
  run. Relevant points: all narrowing is an explicit `static_cast` after clamping, there are
  no unused parameters, and the tests build with `/utf-8` for the UTF-8 literals. Windows CI
  is the real check.
- `chosen_sources` are deduplicated case-sensitively, since the contract is silent. If OBS
  source UUIDs can differ only in case, `validated()` should lowercase them.
- The game-rule record keys (`executable`, `display_name`, `ignore`) are string literals in
  the anonymous namespace of the `.cpp`, because the header has no constants for them. The
  tests spell the keys out, so a typo fails a test.
- `docs/PLAN.md` and WP2.5 define three save-sound settings (`play_save_sound`,
  `save_sound_path`, `save_sound_volume`) that `Settings` does not have. They need a header
  change, which is out of scope for this WP.
- The round-trip test customises all 16 fields other than `schema_version`. That field
  cannot round-trip by design, because `save_settings()` always writes the current version.

## Questions for the orchestrator

1. Should `sanitize_folder_name()` be fixed in `clip-layout`? The fix is to place the `_`
   before the first `.` (`COM1.exe` -> `COM1_.exe`). That is idempotent, and it changes the
   expectation at `tests/test-clip-layout.cpp` line 48 (`"COM1.exe"` -> `"COM1_.exe"`). Who
   owns `clip-layout`, and should a follow-up WP take it?
2. Should the three save-sound fields from WP2.5 be added to `Settings` now, or in M2 when
   WP2.5 lands?
3. Should the game-rule record keys move into `settings-model.hpp` so that
   `obs/settings-store` can share them?
4. Are OBS source UUIDs always lowercase? If not, `chosen_sources` should be lowercased in
   `validated()`.

## Orchestrator integration notes

- Reviewed the implementation in full. Gates re-run in the main checkout: check-format clean, full suite green, Ubuntu CI-preset plugin build OK.
- Answers:
  1. **The clip-layout bug is real, and worse than non-idempotence.** Windows treats `COM1.<anything>` as a device name, so `COM1.exe_` is still reserved. The orchestrator fixes `sanitize_folder_name` in the next commit (`_` goes on the stem: `COM1_.exe`) and turns the excluded idempotence case into an assertion.
  2. The save-sound fields come with M2's interface headers (WP2.0), not now.
  3. The rule record keys stay private to `settings-model.cpp`. The store adapts records generically, so it doesn't need them.
  4. OBS generates source UUIDs as lowercase hex, so case-sensitive dedup is correct.
