# WP5.1 hand-over: core/version and core/update-policy (worker)

## Summary

Implemented every declaration in `src/core/version.hpp` and `src/core/update-policy.hpp`,
in pure C++17 inside `namespace clip_atlas`, with no OBS, Qt or Win32 includes. Both
headers are unchanged. The base was checked with
`git merge --ff-only claude/pensive-hamilton-cbg1l4`, which left the worktree at
`217b811 WP5.0: M5 interface headers and locale keys`. Nothing is committed or pushed;
the new files are staged with `git add -A`.

- `parse_version`: an optional single `v` or `V`, then MAJOR.MINOR.PATCH. Each number is
  capped at 999999999 and checked before every multiplication, so a 32-bit `unsigned long`
  cannot wrap. An optional `-` pre-release and `+` build follow. The build is validated,
  then dropped. Identifiers are non-empty and made of `[0-9A-Za-z-]`, and a numeric
  pre-release identifier may not have leading zeros.
- `compare_versions`: semver 2.0 section 11 precedence. Numeric identifiers are compared
  as digit strings, never converted to an integer, so long numbers are exact. The result
  is always -1, 0 or 1.
- `is_newer_version` and `format_version`: as the header describes.
- `update_check_due`: due at exactly 24 h. A stored time more than 3600 s ahead of `now`
  is due; exactly 3600 s ahead is not. No `int64_t` input can overflow.
- `is_trusted_release_url`: the exact prefix, then one or more of `[0-9A-Za-z._~/-]`, no
  `..`, and at most 300 bytes.
- `evaluate_update`: the header's order of checks. The error strings are the header's,
  with the values filled in.

## Files changed

| File | Change |
|---|---|
| `src/core/version.cpp` | new |
| `src/core/update-policy.cpp` | new; defines `kLatestReleaseApiUrl` and `kReleasesPageUrl` |
| `tests/test-version.cpp` | new; 25 test cases, 1153 assertions |
| `tests/test-update-policy.cpp` | new; 28 test cases, 662 assertions |
| `docs/handover/WP5.1.md` | new (this note) |

No other file was touched. `git status` shows only these five.

## How it was tested

1. `cmake -S tests -B build/tests`: configures. Catch2 v3.7.1 is fetched by FetchContent.
2. `cmake --build build/tests`: succeeds. The rebuild log has zero warnings.
3. `ctest --test-dir build/tests --output-on-failure`: 100% passed, 299 of 299. The test
   binary also holds the other packages' tests; the 53 test cases from this package are
   included.
4. The two new files, selected by name:
   - `parse_version*,compare_versions*,is_newer_version*,format_version*`: "All tests
     passed (1153 assertions in 25 test cases)".
   - `update_check_due*,is_trusted_release_url*,evaluate_update*,the endpoint*,UpdateResult*`:
     "All tests passed (662 assertions in 28 test cases)".
5. Strict warnings, outside CMake: g++ 13.3 and the system clang++, with
   `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion`, on the four C++
   files: zero warnings.
6. `clang-format-19 --style=file --dry-run --Werror` on the four C++ files: clean
   (clang-format 19.1.1).
7. `git add -A && build-aux/check-format`: "all 73 C/C++ and 16 CMake files are formatted."
8. Mutation check, in scratch copies only (not in the repo): 20 deliberate faults, each
   applied to a copy of `version.cpp` or `update-policy.cpp` and compiled against the
   unchanged test files. All 20 made the suite fail, and the unmodified baseline passed.
   The faults were:
   - leading zeros accepted, in numbers and in numeric pre-release identifiers;
   - the cap raised to 4294967295, or checked only before the last digit;
   - numeric identifiers compared as text, or ranked above alphanumeric ones;
   - a shorter identifier list not ranked lower;
   - build metadata not validated;
   - a pre-release ranked above its release;
   - the prefix without its trailing slash, `?` allowed, `..` allowed, a 301-byte limit;
   - exactly 24 h not due, exactly one hour ahead repaired;
   - naive subtraction in `update_check_due`;
   - a transport error ignored, a 404 treated as a failure, an untrusted page offered,
     an equal version offered as an update.

   The script is in the worker's scratch directory and is not part of the repo.

## Deviations and interpretations

1. The bare prefix is rejected. `is_trusted_release_url` refuses
   `https://github.com/sxnfer/auto-clip-sorter/releases/` and any URL that ends right
   after the prefix. The header's "nothing but [0-9A-Za-z._~/-] after that prefix" would
   allow an empty tail; the brief asks for at least one character.
2. `+` is outside the header's character set. A page URL with build metadata, such as
   `.../releases/tag/v1.0.0+build`, is untrusted, so the fallback page is offered instead.
   The release workflow uses bare tags, so no current release is affected.
3. `kReleasesPageUrl` is not accepted by `is_trusted_release_url`, because it has no
   trailing slash. This is what the brief asks for. It is the fallback value of
   `download_url`; see question 1.
4. Build metadata may have leading zeros: `1.0.0+01` is valid. Semver allows this, and the
   header's leading-zero rule covers only numeric pre-release identifiers. A test pins it.
5. Numeric pre-release identifiers have no length cap. The header caps only MAJOR, MINOR
   and PATCH, so a pre-release with a 40-digit number is valid and compares exactly.
6. The `kReleasesPageUrl` comment in the header says "no usable html_url". I read "usable"
   as "trusted", which matches the `evaluate_update` comment.
7. `compare_versions` accepts hand-built `Version` values whose pre-release text
   `parse_version` would refuse, such as empty identifiers. It never throws and gives a
   fixed order, but that order is not part of semver. The tests check only that it does
   not throw and that it is antisymmetric.

## Known gaps and risks

- Windows and MSVC are not built here; this package was checked on Linux only. On Windows
  `unsigned long` is 32 bits. The code never multiplies past the cap, so it cannot wrap.
  That was reasoned, not executed. CI should confirm it.
- The "unexpected tag" error echoes `tag_name` verbatim. Its length and bytes come from
  the JSON answer. WP5.5 should show it as plain text, not rich text, and may truncate it.
- `is_trusted_release_url` checks the string only. It does not follow redirects. WP5.2
  decides what HTTP does with the URL.
- The character set allows `.` segments and `//`. They cannot leave the releases path,
  because `..`, `%`, `?`, `#` and `\` are refused.
- No TODOs are left in the code.

## Questions for the orchestrator

1. WP5.5: the glue must open `UpdateResult::download_url` as returned. It must not pass that
   value through `is_trusted_release_url`, or the fallback `kReleasesPageUrl` would be
   refused. Please confirm this is the design.
2. Are the bare-prefix rejection (deviation 1) and the `+` fallback (deviation 2) intended?
3. Should `evaluate_update` truncate or escape the tag in the error string, or leave that to
   the UI? I left it verbatim, since the header's `<tag>` implies a plain substitution.

## Orchestrator integration notes

- Read the whole diff; only the WP's files changed. Gates: 299/299 unit tests, plugin build, check-format clean.
- Answers: WP5.5 opens `UpdateResult::download_url` exactly as returned (the fallback is a constant). The
  bare-prefix rejection and the `+` fallback are intended. The tag stays verbatim in the error; the UI shows it as
  plain text.
