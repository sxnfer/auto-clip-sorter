# WP3.1 hand-over: core/clip-index

## Summary

Every function and the `ClipIndex` class declared in `src/core/clip-index.hpp` are implemented, with no
header changes. The unit tests are in `tests/test-clip-index.cpp`: 43 Catch2 test cases, 378 assertions.
The code covers the clips-folder scan, `describe_clip`, the newest-first `ClipIndex` with filter and
search, the display helpers (age, local date, size) and rename validation.

Pure C++17 with no OBS, Qt or Win32 headers. The only disk I/O is in `scan_clips_folder` and
`describe_clip`, which use `std::error_code` overloads throughout. `ClipIndex` and the helpers do no I/O.

## Files changed

- `src/core/clip-index.cpp` (new, 518 lines)
- `tests/test-clip-index.cpp` (new, 867 lines)
- `docs/handover/WP3.1.md` (new, this note)

The three files are staged with `git add -A`. Nothing is committed. A scratch `build-configure.log` that I
had written into the worktree root was removed before staging. `build/` is git-ignored.

## How it was tested

- Setup: `git merge --ff-only claude/pensive-hamilton-cbg1l4`, then `git log -1 --oneline` shows
  `11b9311 WP3.0: format CMakeLists.txt`.
- `cmake -S tests -B build/tests`: configured, and Catch2 v3.7.1 was fetched.
- `cmake --build build/tests`: built with no errors and no warnings (g++ 13.3, `-Wall -Wextra -Wpedantic`).
- `ctest --test-dir build/tests --output-on-failure`: 289 of 289 pass, including the 43 new cases.
- `build/tests/clip_atlas_tests "[clip-index]"`: All tests passed (378 assertions in 43 test cases).
  Three repeat runs gave the same result.
- `clang-format-19 --style=file --fallback-style=none --dry-run --Werror src/core/clip-index.cpp tests/test-clip-index.cpp`:
  clean.
- `git add -A && build-aux/check-format`: "all 66 C/C++ and 16 CMake files are formatted." The pinned
  gersemi 0.21.0 and clang-format 19 were used.
- Not run: the Windows/MSVC build and test run. Only CI can check that (see the risks).

What the tests cover:

- `path_to_utf8`: a round trip of a non-ASCII name.
- `is_clip_file`: all 11 extensions, upper and mixed case, the partial suffix, no extension, and the
  `".mkv"` hidden-only name. That name is not a clip, because the leading dot is part of the name and
  `extension()` is empty. A scan skips it anyway.
- `kind_of_clip`: prefix case, empty and custom prefixes, images.
- `game_folder_of`: depths 1 to 4, a trailing separator on base, base itself, a file outside base, the
  sibling `D:/Clips2/x.mkv`, `.` and `..` normalisation, non-ASCII folders.
- `describe_clip`: a regular file, a missing file, a folder, a wrong extension, a partial copy, too deep,
  a hidden file and a file in a hidden folder, base itself, a symlink (skipped if the OS refuses to
  create one), non-ASCII names.
- `scan_clips_folder`: a three-level tree with a fourth level, hidden and non-clip entries, order,
  equal modification times broken by path, truncation and its flag, a missing, empty or non-folder base,
  symlinks to files and folders.
- `ClipIndex`: capacity, `reset` dedupe, sort and truncation, zero capacity, `upsert` insert, replace,
  eviction and the `false` return, exact-path `remove` and `find`, `games` order, `has_unfiled`, query
  game filter (including unfiled-only), search with several terms, case, matching on the game folder,
  and blank search.
- `describe_age`: every boundary from 59 s to 7 d, and a time after now.
- `format_local_date`: built with `mktime`.
- `format_size`: every example in the brief, plus 9.9 GB, 1.1 GB, 1.0 TB, 5.0 TB, 2048 TB, and the
  largest `uintmax_t`, which gives `16777215 TB`.
- `renamed_file_name`: each rejection rule, trimming, the extension kept, no extension, non-ASCII stems,
  the same name refused, and the 255-byte limit (ASCII, two-byte characters, and a long extension).

## Deviations from the contract, and why

1. **Scan reads the clocks once.** `to_system_time` reads both clocks on every call. The two reads
   differed by up to 76 ns in this sandbox, and the offset between the clocks is constant. So two files
   with the same file time converted to slightly different `modified` values, and the path tie-break never
   applied. The equal-time test failed on its first run. Fix: `scan_clips_folder` reads both clocks once
   (`ClockReading`) and converts every file against that reading, through `describe_clip_with` and
   `walk_folder`. `to_system_time` and the public `describe_clip` still apply the literal formula, one
   reading per call. Within one scan, equal file times now stay equal, as the header requires.
2. **`describe_clip` also refuses hidden names below base.** A file whose name, or a folder between base
   and the file, starts with `.` returns nullopt. The header says nullopt for "not in a place a scan
   would list", and a scan never lists hidden entries. The brief's list of checks did not include this.
3. **`renamed_file_name` is stricter for device names.** The part before the first dot is compared after
   its trailing spaces are removed, so `"NUL .txt"` and `"Com5 .log"` are refused. The literal rule would
   accept them. This matches `clip-layout`'s device check.
4. **`game_folder_of` with an empty base returns nullopt.** The literal rule does not say what an empty
   base means. An empty base names no folder, so nothing is inside it.
5. **`walk_folder` catches `fs::filesystem_error`.** The `error_code` overloads are used everywhere. The
   catch only keeps "never throws" true if a library call throws anyway.
6. **A folder that fails part-way keeps the entries read before the error.** The header says entries
   that cannot be read are skipped. Entries already read are valid, so they are kept.
7. **Base itself may be a symlink or junction.** `is_directory` follows it, so its contents are listed.
   The no-follow rule applies to entries below base only.

## Known gaps and risks

- **Windows is not compiled or run here.** Watch these in CI:
  - MSVC `/W4 /utf-8`.
  - The symlink tests use SKIP when the OS refuses to create a link (no privilege).
  - Junctions are not tested. I expect `symlink_status` not to report them as directories, so they are
    skipped, but I have not verified this.
  - File times are 100 ns on Windows. The tests set exact stamps, so this does not affect them.
- **Permission-denied folders are not tested.** The sandbox runs as root, which reads everything.
- **`modified` is not stable to the nanosecond across scans.** Each scan reads the clocks once, so the
  same file can get a `modified` value that differs by tens of nanoseconds from one scan to the next.
  `ClipEntry::operator==` compares `modified`, so it would report such entries as changed. The dock should
  not use `==` to detect changes. See question 1.
- **`format_local_date` does not check `localtime`'s return value,** the same as `clip-layout`'s
  `year_month_of`. On Windows a time before 1970 would print `1900-01-00`. Clip times are not that old in
  practice. This is not tested.
- **`scan_clips_folder` collects every clip under base before it truncates.** Memory grows with the number
  of clips on disk. `max_clips` only limits what is kept.
- **`is_clip_file` matches the partial suffix case-sensitively,** as file-mover writes it.
- **`kind_of_clip` is a plain prefix test.** With the default prefix, `"Replayer.mkv"` counts as a replay.
  This is the literal rule.
- **The `format_size` header comment** lists `"1.2 GB"` as a format example. By the floor rule, 1288490188
  bytes (1.1999 GiB) reads `"1.1 GB"`, and 1288490189 reads `"1.2 GB"`. The tests assert the floor result.
- **`ClipIndex` is not thread-safe,** as the header says.

## Questions for the orchestrator

1. **Clock handling.** I kept `ClockReading`, which is a deviation from the literal `to_system_time` formula.
   The other option is a process-wide offset captured once. That would make `modified` stable across scans
   and would also remove the `==` problem, but it changes the formula, so I did not make it without your
   approval.
2. **Hidden names.** Please confirm that `describe_clip` should refuse hidden names below base (deviation 2).
3. **Dock change detection.** The dock should compare entries without `modified`, or use it at one-second
   precision.
4. **TODOs.** There are none in `src/core/clip-index.cpp` or `tests/test-clip-index.cpp`. No dependencies were
   added, and `tests/CMakeLists.txt` was not changed (it globs both files).

## Orchestrator integration notes

- Read the whole diff; only the WP's files changed. Gates: 342/342 unit tests, check-format clean.
- Question 1, fixed by the orchestrator: `read_clocks()` now reads both clocks once per process (a function-local
  static), and the `to_system_time` comment in the header says so. Equal file times stay equal across scans, and a
  filed clip's entry matches what a later scan builds, so `ClipEntry::operator==` is usable by the dock.
- Deviations 2-7 are accepted (hidden names below base are refused by `describe_clip`, as a scan would skip them).
