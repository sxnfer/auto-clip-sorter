# WP1.1 hand-over: core/name-resolver (worker)

## Summary

Implements the pure C++17 resolver declared in `src/core/name-resolver.hpp`: executable
normalization and stem, generic product name check, window-title cleaning, and `resolve_game`.
The header is unchanged. The `.cpp` includes only `<array>`, `<cstddef>`, `<string>` and
`<string_view>`. No dependencies were added.

Base: fast-forwarded to `94b3fd0 Plan: add optional save sound (WP2.5)` as the brief required.

## Files changed

| File | Change |
|---|---|
| `src/core/name-resolver.cpp` | New. Implementation. Pure ASCII source; the en/em dashes and (R) signs are written as UTF-8 escapes. |
| `tests/test-name-resolver.cpp` | New. 38 Catch2 test cases. |
| `docs/handover/WP1.1.md` | New. This note. |

Not touched: `src/core/name-resolver.hpp`, `tests/CMakeLists.txt`, `CMakeLists.txt`, `legacy/`, and
everything else. Nothing was committed or pushed. `build/` and `build_x86_64/` are gitignored.

## Testing

| Check | Command | Result |
|---|---|---|
| Unit tests | `cmake -S tests -B build/tests && cmake --build build/tests -j4 && ctest --test-dir build/tests --output-on-failure` | 49/49 pass: 11 existing clip-layout, 38 new |
| Plugin build, CI preset (`CMAKE_COMPILE_WARNING_AS_ERROR=ON`) | `cmake --preset ubuntu-ci-x86_64 && cmake --build --preset ubuntu-ci-x86_64` | Builds. `name-resolver.cpp` is compiled and linked into `clip-atlas.so`. |
| Format | `clang-format-19 --dry-run --Werror src/core/name-resolver.cpp tests/test-name-resolver.cpp` | Clean |
| Strict warnings | `g++` and `clang++ -std=c++17 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast -Werror -fsyntax-only` on both files | Clean |
| ASCII-only source | grep for non-ASCII bytes in `name-resolver.cpp` | None |

One bug was caught by the tests and fixed. `remove_launcher_suffix` trimmed leading whitespace, so
`" - Steam"` lost the separator's leading space and was not removed. It now ignores only trailing
whitespace when matching.

Coverage: every example in the contract; each resolution step winning in turn; an empty rule name
falling through while `ignore` still applies; an ignored game still getting a name; case-insensitive
exe matching (`ELDENRING.EXE`); an `unknown` exe with a title; everything empty gives `none`; generic
product names skipped, including padded ones; each launcher name and each separator; repeated
suffixes; version-token forms and non-version numbers; UTF-8 titles; and a no-throw sweep over all
256 byte values.

Not verified here: the MSVC build (`/W3 /utf-8 /WX`). No MSVC is installed in this container. The
code avoids `<cctype>`, implicit narrowing and signed/unsigned comparisons, and the `.cpp` is
ASCII-only, so it should be clean. The Windows CI run is the first real check.

## Deviations from the contract

The header is authoritative and unchanged. Three places where the contract text is ambiguous, or
where I chose a reading. Each is covered by a test.

1. **Trim again after the last path separator** (`normalize_executable`, `executable_stem`). The
   contract trims, then takes the last component. I trim again, so `C:\Games\ EldenRing.exe` gives
   `eldenring.exe` instead of `" eldenring.exe"`, and `C:\Games\ UNKNOWN` still counts as unknown.
2. **Rejoined titles have single spaces only** (`clean_window_title`, step 2). "Rejoin with single
   spaces" is read as: empty tokens from runs of spaces are dropped too, so `Hades  Game` gives
   `Hades Game`. Keeping the empty tokens would preserve the double space.
3. **A blank folder key skips the map lookups** (`resolve_game`). Step 1 of the contract reads
   literally as `rules[folder_key]`. For a blank key I skip both the rule and the bundled lookup, so
   no entry stored under `""` can match. `ignored` stays false for a blank key, which the contract
   already implies.

## Known gaps and risks

These follow from the contract's fixed step order or its grammar. I kept them as specified and did
not pin them in tests.

- **Suffix before version.** Launcher suffixes are removed before version tokens, so
  `Celeste - Steam 1.0` gives `Celeste - Steam`. Likewise `Game - Steam -` gives `Game - Steam`.
  Both were checked with a scratch program outside the repo.
- **Tabs do not split tokens.** The contract says to split on ASCII spaces, so `Game<TAB>1.0` stays
  as is. Tabs are still trimmed at the ends of a title.
- **Dotted numbers that belong to a name are dropped.** `Mod 2.0` gives `Mod`. This is the cost of the
  "a dot is required" rule. Undotted numbers are kept (`Doom 3`).
- **Some near-miss versions are kept.** `1.0-beta.2` and `1.0_beta` are not stripped, because the
  tail allows only letters, digits and `-`.
- **Only ASCII case folding.** Non-ASCII letters keep their case in comparisons, as specified.
- **Exact generic-name list.** `Microsoft Windows Operating System` without the (R) signs is not
  treated as generic, because the contract says exact matching.

## Questions for the orchestrator

1. Should the suffix and version steps repeat until neither changes, so that `Celeste - Steam 1.0`
   gives `Celeste`? That is a small change inside `clean_window_title`. I kept the fixed order
   because the contract states it.
2. Do you accept the three readings under "Deviations from the contract" (trim again after the
   separator, single-space rejoin, blank key skips lookups)? Each is a one-line change plus a test
   if you want the literal reading.
3. Should tabs also split tokens in titles? The contract says spaces, and I followed it.

## Orchestrator integration notes

- Reviewed the implementation in full. Gates re-run in the main checkout: check-format clean, full suite green, Ubuntu CI-preset plugin build OK.
- Answers: (1) Keep the fixed suffix-then-version order; a title like `Celeste - Steam 1.0` is unlikely and the gap is documented above. (2) All three readings are accepted: trim again after the separator, single-space rejoin, blank key skips lookups. (3) Tabs don't split tokens.
