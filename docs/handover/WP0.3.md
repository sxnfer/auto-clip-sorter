# WP0.3 hand-over: CI unit tests and format check

**Status:** done locally. The Windows test build, the apt and pipx steps and the whole workflow run are unverified on GitHub, because no runner is available here. See known gaps.

## Summary

- Every push (including `claude/**`) now runs the format check. The `main`/`master` condition is gone from `push.yaml`.
- A new reusable `unit-tests.yaml` builds `tests/` with CMake and runs `ctest` on `ubuntu-24.04` and `windows-2022`. `push.yaml` and `pr-pull.yaml` both call it.
- `build-aux/check-format` replaces the two changed-files actions. It checks every git-tracked C/C++ file with clang-format 19 and every CMake file with gersemi 0.21.x, so the check covers the whole repo. `--fix` rewrites what fails.
- The repo is compliant. `build-aux/check-format` exits 0. The only changes are formatting: three one-line lambdas expanded (one in `src/core/clip-layout.cpp`, two in `tests/test-clip-layout.cpp`), and one `FetchContent_Declare` call collapsed in `tests/CMakeLists.txt`.
- The three changed-files actions are deleted. Nothing references them now, so the `$GITHUB_OUTPUT` bug in `check-changes` no longer exists in the repo.

## Files changed

Owned paths only. Nothing is staged or committed.

- **Added:** `build-aux/check-format` (mode 755), `.github/workflows/unit-tests.yaml`, `docs/handover/WP0.3.md`.
- **Modified:**
  - `.github/workflows/check-format.yaml`: one job on `ubuntu-24.04`. It installs `clang-format-19` and `pipx`, then `pipx install gersemi==0.21.0`, then runs `build-aux/check-format`.
  - `.github/workflows/push.yaml`: removed the `if:` from `check-format`. Added a `unit-tests` job with `permissions: contents: read`.
  - `.github/workflows/pr-pull.yaml`: added the same `unit-tests` job.
  - `src/core/clip-layout.cpp`: formatting only (`clang-format-19 -i`).
  - `tests/test-clip-layout.cpp`: formatting only (`clang-format-19 -i`).
  - `tests/CMakeLists.txt`: gersemi formatting only. See deviation 3.
- **Deleted:** `.github/actions/run-clang-format/`, `.github/actions/run-gersemi/`, `.github/actions/check-changes/`. The only remaining textual mentions are in `docs/handover/WP0.1.md`, which is history and was left alone, and in this note.
- **Not touched:** `build-aux/.run-format.zsh` and its three symlinks, `src/core/clip-layout.hpp` (already compliant), and every read-only path.

For the orchestrator's commit: stage the three deletions, the two new files and the modified files. Check that `git ls-files -s build-aux/check-format` shows mode `100755` after `git add`.

## How it was tested

All commands were run from the worktree.

| Check | Command | Result |
|---|---|---|
| Baseline detection | `build-aux/check-format` on the untouched tree | Exit 1. Named `src/core/clip-layout.cpp`, `tests/test-clip-layout.cpp` and `tests/CMakeLists.txt`, with a diff for each. |
| Fix | `build-aux/check-format --fix` | Rewrote exactly those three files. `git status` shows no other change. |
| Compliance | `build-aux/check-format` | Exit 0. `all 5 C/C++ and 16 CMake files are formatted.` About 5 s. |
| Fix is idempotent | `build-aux/check-format --fix` on the clean tree | Exit 0, no rewrites. |
| Breakage detection | Added trailing spaces to `tests/test-clip-layout.cpp` and `tests/CMakeLists.txt`, then ran the check | Exit 1, naming both files. Both restored from backups (`cmp` identical), then check exit 0. |
| Parse error | Appended `set(BROKEN` to `tests/CMakeLists.txt` | Exit 1 with `gersemi could not process` and gersemi's message. Restored, check exit 0. |
| Argument handling | `--help`; `--bogus`; `--fix extra` | Exit 0; exit 2; exit 2. |
| Version refusals | `CLANG_FORMAT=/usr/bin/clang-format` (18.1.3); a fake `gersemi` 0.20.1 first on PATH; PATH without gersemi | Exit 2 each, with a message naming the problem. |
| No files matched | `GIT_INDEX_FILE` pointing at an empty index in the scratchpad | `no tracked C/C++ or CMake files, nothing to check.` Exit 0. |
| Legacy exclusion | `git ls-files -- '*.py'`, then with `':!legacy/'` | First lists `legacy/rb_event_driven.py`. Second lists nothing. |
| Script syntax | `bash -n build-aux/check-format` | Exit 0. |
| Unit tests, clean | `rm -rf build/tests`, then `cmake -S tests -B build/tests && cmake --build build/tests --config Release -j4 && ctest --test-dir build/tests -C Release --output-on-failure` | All three exit 0. `100% tests passed, 0 tests failed out of 11`. |
| Deliberate failure | Appended `TEST_CASE("deliberate failure") { CHECK(false); }` to `tests/test-clip-layout.cpp`, rebuilt, ran ctest | ctest exit 8. `12 - deliberate failure (Failed)`. File restored from backup, rebuilt, 11/11 again. |
| Multi-config flow (Windows-like) | `cmake -S tests -B build/tests-mc -G "Ninja Multi-Config"`, then `cmake --build build/tests-mc --config Release`, then `ctest --test-dir build/tests-mc -C Release` | Built `Release/clip_atlas_tests`. 11/11 pass. This is a Linux stand-in, not the VS generator. |
| YAML parse | `python3 -c "import yaml,glob; [yaml.safe_load(open(f)) for f in glob.glob('.github/**/*.yaml', recursive=True)]"` | All 8 files parse. |
| Workflow references | Scratch checker (not in the repo). Every `uses: ./...` resolves, every `needs:` names a real job, every called workflow declares `workflow_call`. | Passes. A copy with one broken `uses:` path fails with exit 1. |
| Whitespace-only C++ edits | Stripped all whitespace from HEAD and working copies, then compared. See deviation 2. | Identical for `clip-layout.cpp`, `test-clip-layout.cpp`, `clip-layout.hpp` and `tests/CMakeLists.txt`. |

## Deviations from the contract

1. **Worktree base was stale.** The worktree branch was at `77f7bcd`, not `claude/m0-scaffold` (`faf62e5`), so the template files were missing. The branch was three commits behind and had no commits or local changes of its own. I ran `git merge --ff-only claude/m0-scaffold` in this worktree and stayed on the same branch.
2. **Step (e) expectation does not hold as written.** `git diff -w --stat src/core tests/test-clip-layout.cpp` is not empty. `-w` ignores whitespace within a line but not line breaks, and a lambda expansion adds line breaks. `git diff -w --stat` reports the same 9 insertions and 3 deletions as the plain diff. The property that matters is that only whitespace changed, and that holds: the whitespace-stripped comparison above is identical for both files.
3. **`tests/CMakeLists.txt` needed no non-formatting change for MSVC.** Catch2 v3.7.1's `catch_discover_tests` runs in POST_BUILD mode, and that writes one `clip_atlas_tests-<hash>_tests.cmake` file shared by all configurations. The Ninja Multi-Config run above exercises this path. Its test list resolves to the `Release` executable and `ctest -C Release` picks it up.
4. **Per-file tool invocations.** The contract asked for `--dry-run --Werror`, and that is what runs. I call the tools one file at a time, not in one batch, so every failing file is named exactly and gets its own diff. gersemi exits 1 for "would be reformatted" but 123 for a parse error, so the script treats 1 as a formatting violation and anything else as a failure with gersemi's own message. gersemi runs with `--no-cache` so a cached result can never mask a violation.
5. **pipx, installed through apt.** `pip install --user` is refused on Ubuntu 24.04 (PEP 668), so gersemi goes in its own pipx environment. I install `pipx` from apt in the same step rather than assuming the runner image has it. `clang-format-19` (19.1.1) and `pipx` (1.4.3) were confirmed as apt candidates on this Ubuntu 24.04.5 base.
6. **Windows command is exactly as the contract gives it.** No `-A x64` was added. See question 3.
7. **"Nothing references them" was read as "no code uses them".** A literal `grep -rn` still matches the old action names in `docs/handover/WP0.1.md` and in this note. Nothing executable refers to them, so the directories were deleted and the history was left alone.

## Known gaps and risks

- **GitHub Actions not run.** The apt and pipx steps, both matrix legs and the reusable-workflow wiring were checked only by reading the YAML and by the local checker. The Windows build is not verified. The Ninja Multi-Config run is a stand-in for MSVC.
- **Windows platform.** Without `-A x64`, the Visual Studio generator may pick Win32 on `windows-2022`. I believe the default is Win32 but did not confirm it. I expect the tests to compile and pass as Win32 too, but that is unverified, and x64 coverage depends on the default.
- **Markdown-only pull requests skip the checks.** `pr-pull.yaml` keeps `paths-ignore: '**.md'` from the template. A PR that touches only Markdown runs neither the format check nor the unit tests.
- **Only tracked files are checked locally.** `git ls-files` lists the index, so a new file is checked only after `git add`. CI checks out committed content, so it sees everything.
- **Parallel work.** The root `CMakeLists.txt` and `src/plugin-main.cpp` are not mine. If they break gersemi or clang-format, `check-format` goes red on the integrated branch, so their owner must run `build-aux/check-format --fix` first.
- **Unused template script.** Nothing in CI calls `build-aux/.run-format.zsh` or its symlinks now. I left them in place as instructed.
- **Open WP0.1 template bug, not mine.** The `package-plugin/action.yaml` Ubuntu packaging step still appends `--debug` to `build_args` instead of `package_args`. That file is read-only for this WP.
- **No caching.** Catch2 is fetched from GitHub on every run, as the contract asked.

## Questions for the orchestrator

1. **Other worktree.** `worktree-agent-af28621c800ab3e39` is also locked at `77f7bcd`, the stale base. I did not touch it. Its owner needs the same fast-forward before starting.
2. **Markdown-only PRs.** Keep `paths-ignore: '**.md'` on the PR workflow, or drop it so every PR to `main` gets the checks, as the brief literally says?
3. **Windows x64.** Should the Windows unit-test step pass `-A x64`? It needs a separate command or a matrix conditional, because `-A` breaks the Linux leg.
4. **Remove `build-aux/.run-format.zsh` and its symlinks** in a later cleanup now that no workflow uses them?
5. **Step (e).** Accept the whitespace-stripped comparison in place of `git diff -w --stat` for the two C++ files?

## Orchestrator integration notes

- Reviewed the full diff; the C++ changes are formatting only and all files are inside ownership. Gates re-run on the integrated tree (with WP0.2): `build-aux/check-format` clean (5 C/C++, 16 CMake), unit tests 11/11, Ubuntu CI preset build OK.
- Answers: (1) the other worktree had been fast-forwarded too. (2) Keep `paths-ignore: '**.md'`. (3) No `-A x64`: the Visual Studio 17 2022 generator defaults to the host platform, x64 on the runner; to be confirmed in the CI log. (4) Leave `.run-format.zsh` for now. (5) The whitespace-stripped comparison is accepted.
- The "failing test turns CI red" acceptance check is done by the orchestrator with a throwaway commit; see the result below.
