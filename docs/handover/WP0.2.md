# WP0.2 hand-over: Plugin entry point, locale, LICENSE

**Status:** done locally. The Ubuntu CI-preset build passes with warnings as errors, and a headless smoke load of the module logs the expected line on OBS 30.0.2. The Windows (MSVC) build was not run here, so CI is the check for that. One gap in incremental builds is listed under known gaps (gap 1).

## Summary

- `src/plugin-main.c` is replaced by `src/plugin-main.cpp`. `obs_module_load` logs `[clip-atlas] loaded v0.1.0` and returns true. `obs_module_unload` logs `unloaded`. `obs_module_name` and `obs_module_description` return the `ModuleName` and `ModuleDescription` strings from the locale. Load and unload catch every exception and log it, and load returns false if it caught one.
- `data/locale/en-US.ini` holds the 14 starter keys from the contract. It is UTF-8 with no BOM and LF line endings.
- `LICENSE` is the GPL v2 text from the obs-plugintemplate, copied byte for byte. sha256 `90b664a6fbc82b38595cbb39606268bef0b2eb9b76b0e9e03826befe204190bb`.
- Root `CMakeLists.txt` now globs `src/core`, `src/obs` and `src/ui` (`*.cpp`), the platform variant (`src/platform/*-win.cpp` when `WIN32`, otherwise `*-stub.cpp`), and the `*.hpp` headers of those folders. It adds `src`, `src/core`, `src/obs`, `src/platform` and `src/ui` as include directories. The `ENABLE_FRONTEND_API` and `ENABLE_QT` handling is unchanged.
- Later work packages need no `CMakeLists.txt` edit for new sources or headers. Verified in a scratch copy (see Testing).
- `src/core/clip-layout.cpp` is compiled into the module now. It was not before.

## Files changed

Paths are relative to the repo root. `git status --short` shows exactly these paths.

- `CMakeLists.txt` (modified). Replaced the single `target_sources(... src/plugin-main.c)` with globbed source and header lists, plus `target_include_directories`. Formatted with gersemi.
- `src/plugin-main.cpp` (new). The license header is copied from `src/plugin-main.c` (lines 1-17, confirmed identical with `diff`). It uses `OBS_DECLARE_MODULE()` and `OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")`, as the template does.
- `src/plugin-main.c` (deleted).
- `data/locale/en-US.ini` (modified; the template had it empty).
- `LICENSE` (new).
- `docs/handover/WP0.2.md` (new, this note).

## Testing

| Check | Command | Result |
|---|---|---|
| a. Format | `clang-format-19 --dry-run --Werror src/plugin-main.cpp` | Exit 0 (clang-format 19.1.1). |
| b. CMake format | `gersemi --check CMakeLists.txt` | Exit 0. Ran `gersemi -i CMakeLists.txt` first. The only changes were line wrapping. |
| c. Configure | `cmake --preset ubuntu-ci-x86_64` | Exit 0. One CMake warning: "Manually-specified variables were not used: ENABLE_CCACHE", because ccache is not installed. It is not a failure, so I did not install ccache. |
| c. Build | `cmake --build --preset ubuntu-ci-x86_64` | Exit 0 with `CMAKE_COMPILE_WARNING_AS_ERROR=TRUE`. The log has zero warnings or errors. Compiled `plugin-support.c`, `src/plugin-main.cpp` and `src/core/clip-layout.cpp`, then linked `build_x86_64/clip-atlas.so`. Checked with `ninja -t commands`: `-Werror`, `-std=c++17`, `-Wextra`, `-fvisibility=hidden`. |
| c. Exports | `nm -D --defined-only build_x86_64/clip-atlas.so \| grep obs_module_` | All required symbols are present: `obs_module_load`, `obs_module_unload`, `obs_module_name`, `obs_module_description`, `obs_module_set_locale`, `obs_module_free_locale`, `obs_module_ver`, `obs_module_set_pointer`. Also present: `obs_module_get_string`, which the locale macro adds. |
| c. clip-layout compiled | `nm -C build_x86_64/clip-atlas.so \| grep -c clip_atlas` | 12 symbols, for example `clip_atlas::destination_dir` and `clip_atlas::unique_destination`. The dynamic-table count (`nm -DC ... \| grep -c clip_atlas`) is 0 because `-fvisibility=hidden` hides the namespace. That is intended. |
| c. Log text | `strings build_x86_64/clip-atlas.so` | Contains `loaded v%s`, the `obs_log` prefix `[%s] %s`, `clip-atlas` and `0.1.0`. |
| d. Unit tests | `cmake -S tests -B build/tests && cmake --build build/tests -j4 && ctest --test-dir build/tests --output-on-failure` | Configure and build exit 0 with no warnings. `ctest`: 11/11 passed. |
| e. Smoke load | Scratch harness, not committed: `/tmp/claude-0/-home-user-auto-clip-sorter/7156fddb-48fe-5a2e-b1b6-f895365608ba/scratchpad/wp02-harness/load-test.cpp`. It runs `obs_startup`, `obs_open_module` and `obs_init_module` on `build_x86_64/rundir/RelWithDebInfo/clip-atlas.so`, with the staged `clip-atlas` data dir. Run under `xvfb-run -a`. | `info: [clip-atlas] loaded v0.1.0`, `obs_init_module ok=1`. `obs_module_name()` = "Clip Atlas". `obs_module_description()` = "Sorts replays, recordings and screenshots into per-game folders.". On shutdown, `info: [clip-atlas] unloaded`. |
| Glob behaviour | Scratch copy of `CMakeLists.txt`, `CMakePresets.json`, `buildspec.json`, `cmake/`, `data/`, `src/` and `LICENSE`, in `/tmp/claude-0/-home-user-auto-clip-sorter/7156fddb-48fe-5a2e-b1b6-f895365608ba/scratchpad/wp02-glob-probe`. Nothing added to the repo. Added `src/obs/probe-glob.cpp` (a new folder, includes `clip-layout.hpp` by bare name), `src/platform/probe-clock-stub.cpp`, `src/platform/probe-clock-win.cpp` (includes `windows.h`) and `src/ui/probe-widget.hpp` with `Q_OBJECT`. | Rebuilt without re-running CMake by hand. `CONFIGURE_DEPENDS` re-ran the globs and re-configured. The stub, the new `src/obs` file and the `src/ui` file compiled. `probe-clock-win.cpp` was excluded. A clean build generated `moc_probe-widget.cpp`. Incremental builds have a gap, see gap 1. |
| Missing folders | Configure and build with `src/obs`, `src/ui` and `src/platform` absent (the state in this repo) | Globbing a missing folder gives nothing. Configure and build succeed. |
| Ownership | `git status --short` | `M CMakeLists.txt`, `M data/locale/en-US.ini`, `D src/plugin-main.c`, `?? LICENSE`, `?? src/plugin-main.cpp`, `?? docs/handover/WP0.2.md`. Nothing else. |

Smoke-load notes:
- A plain headless run fails in `obs_startup` with "Unable to open X display", so `xvfb-run` was needed. Xvfb was already installed. Nothing was installed.
- libobs still prints "Unable to open X display" during startup under Xvfb. The load succeeds regardless.

## Deviations from the contract

1. **The worktree base was stale.** The worktree branch `worktree-agent-af28621c800ab3e39` was at `77f7bcd`, from before the WP0.1 import, not at `claude/m0-scaffold`. Before editing anything, I ran `git merge --ff-only claude/m0-scaffold`, which fast-forwarded the branch to `faf62e5`. The branch name is unchanged, and I made no commits. The other worktree, `worktree-agent-a1f605acff7346f97`, is also at `77f7bcd`.
2. **The contract's `nm -DC | grep -c clip_atlas` check returns 0 by design.** The namespace symbols are hidden. The check was done on the static symbol table instead (12 matches). The exports check was done as specified.
3. **ccache is not installed.** The preset still configures, with the warning above. The contract only asked for an install if the preset failed, and it did not fail.
4. **`Log.Loaded` is kept** in `en-US.ini` although the code does not use it, because it is in the contract's starter list.
5. **gersemi was applied with `gersemi -i`.** The change was formatting only.

## Known gaps and risks

1. **AUTOMOC misses a new `Q_OBJECT` header in an existing build tree. This is a real gap, not a bug in this WP.**
   - Adding a new `Q_OBJECT` header to an existing build directory compiles and links with no error. No moc output is generated, so the module has an undefined `ClassName::metaObject()` symbol. Shared modules link with undefined symbols, so the failure would only show at load time.
   - Verified in the scratch copy. Incremental builds miss it, and a fresh build generates the moc output and defines the symbol.
   - Workaround, verified: delete `<build dir>/clip-atlas_autogen/timestamp`. That forces the moc step to run.
   - CI builds are clean, so CI is not affected. Not fixed in this WP. See question 1.
2. **Windows (MSVC) is not built here.** CI on Windows is the real check. This WP adds `src/core/clip-layout.cpp` to the module for the first time, and CI compiles it with `/W3 /WX`. I read the file for narrowing conversions and found none that MSVC flags at `/W3`. That is only a read-through. The unit tests build the same file with MSVC `/W4`, but only once WP0.3 adds that CI job.
3. **OBS version.** The compile and the smoke load used OBS 30.0.2 from Ubuntu. The 31.0.0 pin is exercised only in CI. The APIs used are the same in both, as the contract states.
4. **`ENABLE_QT` and `ENABLE_FRONTEND_API` are not gated.** Both presets turn them on. With either one off, the globbed `src/ui` and `src/obs` folders will fail to compile once they contain code. The template had no gate, so I kept it that way.
5. **The LICENSE is the template's file, byte for byte.** It differs from Debian's `/usr/share/common-licenses/GPL-2` only in the appendix placeholders and one FSF address line. It has no trailing newline, as in the template. The license terms are unmodified GPL v2, and the "or later" comes from the source headers.

## Questions for the orchestrator

1. **AUTOMOC gap (gap 1).** Accept it as a documented gap, with a rule to clean-build after adding a `Q_OBJECT` header? Or should a configure-time fix be added, such as removing `clip-atlas_autogen/timestamp` whenever the header list changes? That touches a CMake-internal path, so I left it out.
2. **Stale worktree base.** Please confirm the fast-forward in deviation 1 is acceptable. The CI worker's worktree, `worktree-agent-a1f605acff7346f97`, is also at `77f7bcd`, so it probably needs the same fix.
3. **Gating.** Should the root CMake fail early with a clear message when `src/ui` or `src/obs` have sources but `ENABLE_QT` or `ENABLE_FRONTEND_API` is OFF? I did not add that, since the brief says to keep the template's handling.
4. **ccache.** Install ccache in the container so the `ubuntu-ci-x86_64` configure is free of warnings? It is not needed for correctness.
5. **`Log.Loaded`.** Keep the unused key, or remove it?

## Orchestrator integration notes

- Reviewed the full diff; it matches the contract. Gates re-run in the main checkout: clang-format-19 and gersemi clean, a clean `ubuntu-ci-x86_64` build (warnings as errors) OK with all 9 `obs_module_*` exports present, unit tests 11/11.
- Removed the unused `Log.Loaded` key from `en-US.ini`; the contract made it optional and unused keys are noise.
- Answers to the questions above:
  1. AUTOMOC gap: accepted and documented in the decision log. CI always builds clean; locally, clean-build after adding a `Q_OBJECT` header.
  2. Stale worktree base: the fast-forward was the right fix. The other worktree had been fast-forwarded too.
  3. No `ENABLE_QT`/`ENABLE_FRONTEND_API` gate; both are always on in our presets.
  4. ccache: not needed.
  5. `Log.Loaded`: removed.
