# WP2.5 hand-over: save sound (core/wav-volume, platform/sound-player, wiring)

## Summary

The save sound is implemented end to end. When a replay has been filed (moved or copied) and the setting is on, the
file's samples are scaled in memory by (volume/100)^2 and played through winmm's `PlaySoundW` with `SND_MEMORY`, on a
worker thread so the UI thread never touches the disk. The scaling is pure C++ with 27 Catch2 tests. The Windows
player is syntax-checked against a Win32 mock and was run on Linux with a fake `PlaySoundW` under AddressSanitizer,
UBSan and ThreadSanitizer. The first real MSVC compile is Windows CI, and the sound itself needs the user's Windows test.

**Integration dependency:** the one-sound-per-save rule needs WP2.3 to give every file of one save the same nonzero
`save_id`. Nothing in `src/obs` sets it yet (see Known gaps).

## Files changed

| File | Kind |
|---|---|
| `src/core/wav-volume.cpp` | new; implements `wav-volume.hpp` |
| `tests/test-wav-volume.cpp` | new; 27 test cases |
| `src/platform/sound-player-win.cpp` | new; Windows player (built on Windows only) |
| `src/platform/sound-player-stub.cpp` | new; non-Windows stub |
| `src/plugin-main.cpp` | `on_clip_sorted` listener, callback registration in `start_plugin()`, `sound_player().shutdown()` in `stop_all()`, 32-entry save-id memory, includes |
| `docs/handover/WP2.5.md` | this note |

No header, `CMakeLists.txt`, settings dialog, `src/obs/*.cpp`, `data/locale/*` or sound file was touched. Nothing was
committed.

## How it was tested

1. Unit tests: `cmake -S tests -B build/tests && cmake --build build/tests && ctest --test-dir build/tests --output-on-failure`.
   Result: 202 of 202 pass (175 existing, 27 new). The build's exit status was checked directly.
2. Core strict warnings: `g++ -std=c++17 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Isrc/core -c src/core/wav-volume.cpp -o /dev/null`
   exits 0 with g++ 13.3 and with clang++ 18.1 (same flags).
3. Plugin build, CI preset: `cmake --preset ubuntu-ci-x86_64 && cmake --build --preset ubuntu-ci-x86_64` exits 0. That
   preset treats warnings as errors, and the build compiles and links `wav-volume.cpp`, `sound-player-stub.cpp` and
   `plugin-main.cpp`. It used Ubuntu's OBS 30.0.2 and Qt 6.4.2, not OBS 31.
4. Windows syntax: `sound-player-win.cpp` with `-fsyntax-only` against a scratch mock of the Win32 names it uses
   (BOOL, LPCWSTR, HMODULE, DWORD, PlaySoundW, SND_*). Exit 0 with g++ and clang++. The only warnings come from OBS
   headers (anonymous structs under `-Wpedantic`). The mock lives in the session scratch directory and is not
   committed.
5. Windows logic on Linux: the real `sound-player-win.cpp`, linked with a fake `PlaySoundW` that records each call and
   reads the playing image on stop, so freeing an image too early shows up as an ASan error. 23 checks cover: scaled
   play, full volume, missing and non-WAV files, `PlaySoundW` returning FALSE, replacing a queued request, shutdown
   with a queued request, play after shutdown, shutdown from the worker's own callback, a throwing callback, a
   900-call stress from three threads, and the `sound_player()` singleton.
   Result: all 23 pass under `-fsanitize=address,undefined`, and all 23 pass under `-fsanitize=thread` with zero
   reports.
6. Test sensitivity: a scratch copy of `wav-volume.cpp` that uses `floor()` instead of `std::round()` fails 6 of the
   27 test cases. A copy without the data-chunk clamp makes AddressSanitizer report a heap-buffer-overflow in the
   test run.
7. Format: `clang-format-19 --dry-run --Werror` is clean on all five C++ files touched.
8. Strict warnings on `sound-player-win.cpp` and `sound-player-stub.cpp` (same flag set): no warnings from this code.

## Deviations from the contract (and why)

1. **`save_id` 0 is not deduplicated.** `ClipSorted::save_id` defaults to 0. If id 0 were deduplicated, the first save with an unset id would play and
   every later one would be silenced. Id 0 therefore always plays. Real ids (WP2.3) make the
   rule work.
2. **`shutdown()` on the worker thread detaches instead of joining.** The contract says to join only when the caller
   is not the worker. If the worker's `std::thread` were left joinable, destroying it later would call
   `std::terminate`. No code calls `shutdown()` from a callback today.
3. **Float samples at 0 %** are written as +0.0f instead of multiplied, because 0 times NaN or infinity is NaN.
   The float path also refuses a host that doesn't store floats little-endian, since `memcpy` into a float would read
   the file wrong there. Every target passes that check.
4. **Volume 100 or more** checks only the size limit and the RIFF/WAVE tags, as the header says ("checking only the
   RIFF/WAVE header"). I read the header's error list as applying below 100 %, so a damaged `fmt` or `data` chunk
   plays unchanged at full volume.
5. **The first `fmt ` chunk is used.** A later one is skipped like an unknown chunk. The header doesn't say.
6. **Zero channels are an error** ("the fmt chunk has no channels"), since block_align would be 0.
7. **A failed `std::thread` start refuses the request** with "could not start the sound thread", so its `done` is
   still called.
8. **`plugin-main.cpp` also includes `clip-filer.hpp`**, because `on_clip_sorted` names `ClipSorted`.
9. **Tests compare byte images with `same_bytes()`**, which returns a bool, instead of `CHECK(a == b)`. Catch2 prints
   byte vectors as text, and in a failing 32-bit comparison its text wrapping asserted and aborted the run. The mutation check hit this.
   Test-only.

## Known gaps and risks

- **Not compiled by MSVC yet.** The first real compile is Windows CI. The plugin target uses `/W3` with warnings as
  errors (`CMAKE_COMPILE_WARNING_AS_ERROR`, on by default), and `/WX` on the link. The conversions were checked by eye
  against `/W3` and by the mock syntax check.
- **Real playback is untested.** The user should check on Windows: the sound plays at the expected level; a second
  replay replaces the first; the sound is audible on a Desktop Audio capture (the caveat the WP2.4 help text states).
- **Integration dependency.** The dedupe needs every `ClipSorted` of one save to carry the same nonzero `save_id`.
  Today the M1 `frontend-events.cpp` doesn't set it, and `clip-filer.cpp` doesn't exist yet. Until WP2.3 sets it, a
  remuxed replay plays its sound twice. Verify this once WP2.3 lands.
- **A file that can't be read or scaled leaves the previous sound playing.** The stop happens only after a successful
  scale, as the contract orders it.
- **Memory:** the scaled copy (at most 10 MB) stays allocated until the next sound or `shutdown()`.
- **Threading for WP2.4:** `done` runs on the player's thread. The settings Test button must hop to the UI thread
  before it touches widgets. The callback in `plugin-main.cpp` only logs.
- **Worktree:** `build/` and `build_x86_64/` were left in place. Both are gitignored.

## Manual checks for the user (suggested for `docs/testing/M2.md`)

1. Enable the save sound, choose a 16-bit PCM `.wav`, and save a replay. The sound plays once, after the clip is filed.
2. Save a replay with a remuxed copy. The sound plays once, not twice (needs WP2.3's `save_id`).
3. Set the volume to 0. The sound is silent. At 100 the file plays as recorded.
4. Point the path at a missing file or a non-WAV file. The log shows one warning and nothing crashes.
5. Save two replays quickly. The second sound replaces the first.

## Questions for the orchestrator

1. Is "save_id 0 always plays" acceptable, or should an unset id be logged once?
2. Confirm the reading of the error list: at volume 100 or more, only the size limit and RIFF/WAVE tags are checked.
3. Confirm that WP2.3's brief requires one nonzero `save_id` on every `ClipSorted` of a save (replays, and every part
   and copy of a recording).
4. Confirm the detach-on-worker choice in `shutdown()`.

## Orchestrator integration notes

- Read the full diff; it stays within WP2.5's ownership. Gates in the main checkout: 202 of 202 unit tests, Ubuntu CI-preset build OK with no undefined `clip_atlas` symbols (WP2.4's `sound_player()` now resolves), `build-aux/check-format` clean.
- Change by the orchestrator: `NOMINMAX` and `WIN32_LEAN_AND_MEAN` are now `#ifndef`-guarded in `sound-player-win.cpp`. Neither is predefined by the build today (`cmake/windows/compilerconfig.cmake` defines only `UNICODE`/`_UNICODE`), but a redefinition would be MSVC warning C4005, an error under `/WX`.
- Answers: (1) save_id 0 always plays: accepted, and WP2.3's brief gives every save a nonzero id from a process-wide counter; (2) yes, at volume >= 100 only the size and the RIFF/WAVE tags are checked; (3) confirmed; (4) detaching when shutdown() runs on the worker is accepted.
