# WP1.3 hand-over: core/replay-policy + core/remux-rules (worker)

## Summary

Both pure functions are implemented against the fixed headers. The headers are unchanged.

- `decide_replay_action()` (contract 1) returns start, stop or none, plus `recheck_at` for a
  pending stop. A stop requires `we_started`. Each start and stop condition is tested on its
  own, and a sweep over every flag combination confirms that nothing stops a buffer we did not
  start.
- `expected_remux_path()` (contract 2) predicts the file OBS's auto-remux writes. It is pure
  path arithmetic with no filesystem access. Literals go through `path_from_utf8`, and
  extensions are compared on native code units with ASCII-only case folding.

One deliberate deviation: the grace period is capped at one year (see Deviations).

## Files changed

| File | Status |
|---|---|
| `src/core/replay-policy.cpp` | new |
| `src/core/remux-rules.cpp` | new |
| `tests/test-replay-policy.cpp` | new, 12 test cases |
| `tests/test-remux-rules.cpp` | new, 14 test cases |
| `docs/handover/WP1.3.md` | new (this note) |

No other file was touched, and both headers are byte-identical to the base. Nothing is
committed. Build output (`build/`, `build_x86_64/`) is gitignored.

## How it was tested

| Check | Command | Result |
|---|---|---|
| Base | `git merge --ff-only claude/m1-feature-parity` | fast-forward to `94b3fd0`, both headers present |
| Unit tests | `cmake -S tests -B build/tests && cmake --build build/tests -j4 && ctest --test-dir build/tests --output-on-failure` | builds with no warnings under GCC 13.3 `-Wall -Wextra -Wpedantic`; 37/37 pass (11 existing clip-layout, 12 replay-policy, 14 remux-rules) |
| Plugin build | `cmake --preset ubuntu-ci-x86_64 && cmake --build --preset ubuntu-ci-x86_64` | configures and links `clip-atlas.so` with `CMAKE_COMPILE_WARNING_AS_ERROR=ON`; both new `.cpp` files compile in it |
| Format | `clang-format-19 --dry-run --Werror` on the four source files | clean (formatted in place with clang-format-19 first) |
| Strict warnings | `g++` 13 and `clang++` with `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast` on the four files | no warnings (MSVC is not available here; see Known gaps) |
| Mutation check | 15 seeded bugs, each in a scratch copy of the implementation | all 15 make the suite fail; the unmodified copy is green |

Seeded bugs that were caught: stop ignoring `we_started`; stop one millisecond early; stop
ignoring `request_pending` or `auto_stop`; start ignoring `suppressed`; no clamp for negative
grace; no clamp for huge grace; a missing `no_game_since` not stopping; AVI not skipped; `.mp4`
not skipped; a case-sensitive `.mp4` check; simple mode honouring the advanced flag; a substring
match in place of the prefix match; no `.remuxed` in the fragmented name; no ASCII case folding.

## Deviations from the contract

1. **Grace period capped at one year.** `clamped_grace()` limits the grace period to
   `[0, 365 days]`. The contract only says that a negative value counts as zero. Without a
   cap, `no_game_since + grace_period` can overflow the clock and wrap into the past, which
   stops the buffer at once. The mutation run confirmed this: with the cap removed,
   `std::chrono::milliseconds::max()` returns `stop` immediately. No realistic setting
   (seconds to minutes) is affected. settings-model (WP1.4) should still clamp; this is a
   backstop.

No other deviations. The stop uses the contract's exact `now >= no_game_since + grace_period`
comparison.

## Known gaps and risks

- **MSVC not run.** The code follows the /W3 rules and is clean under the stricter GCC and
  Clang sets above, but the Windows CI build and test run are the real check.
- **UTF-8 cases ran on Linux only.** On Windows the same tests exercise `wchar_t` paths. The
  Windows CI run is the real check.
- **The stop path depends on the controller clearing `request_pending`.** If OBS never answers
  a start or a stop, neither can happen again until that flag is cleared. WP1.9 needs a
  timeout or an event that clears it.
- **`recheck_at` is only set for a pending stop.** The controller must arm a one-shot timer for
  it. It should also re-run `decide_replay_action()` on every state change, not only at the
  timer.
- **Predicted name only.** `expected_remux_path()` never checks the disk, so it predicts a name
  even when OBS fails to remux. ProRes `.mov` (macOS only) is not modelled, as the header says.

## Notes for merging

- Test-case names start with the function they cover (`decide_replay_action:` or
  `expected_remux_path:`). Catch2 requires unique names across `tests/`, so this avoids clashes
  with other work packages.

## Questions for the orchestrator

1. Is the one-year cap acceptable? If so, `replay-policy.hpp` should mention it. I did not
   touch the header.
2. Should settings-model (WP1.4) clamp `grace_period` to a sensible range, for example 0 to
   300 s?
3. Confirm that WP1.9 owns the `request_pending` timeout described above.

## Orchestrator integration notes

- Reviewed both implementations and the tests in full. Neither test file uses the real clock or sleeps. Gates re-run in the main checkout: `build-aux/check-format` clean, 37/37 tests, Ubuntu CI-preset plugin build OK.
- Answers: (1) the one-year cap is accepted and now documented in `replay-policy.hpp`. (2) settings-model already clamps `grace_period_seconds` to 0–300 by contract. (3) Yes: WP1.9's brief makes the controller clear `request_pending` on a timeout if OBS never answers.
