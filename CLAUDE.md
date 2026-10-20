# Clip Atlas — conventions for every agent

Clip Atlas is a native OBS Studio plugin for **Windows**, written in C++17 and Qt 6 and
targeting OBS 31 or newer.
- What we're building: `docs/PLAN.md`
- How the build is organized, and the work packages: `plan.md`

If you are a worker agent, only change files your brief says you own.

## Layers (strict)
- `src/core/`: pure C++17 and the standard library only. **Never include OBS, Qt or Win32
  headers here.** Every public function needs Catch2 tests in `tests/`.
- `src/platform/`: Win32 code in `*-win.cpp` and a stub in `*-stub.cpp` for each
  interface. The stub must compile on Linux.
- `src/obs/`: thin glue code that calls libobs and obs-frontend-api. Logic belongs in
  `core/`.
- `src/ui/`: Qt Widgets only.
- `legacy/`: the deprecated Python script. **Do not modify it.**

## Code style
- Follow the repo's `.clang-format`: tabs, 120 columns. Run
  `clang-format --dry-run --Werror` on any file you touch.
- Everything goes in `namespace clip_atlas`. File names use kebab-case. Functions and
  variables use `snake_case`, and types use `PascalCase`, as in `src/core/clip-layout.hpp`.
- Write comments in the same density and tone as `clip-layout`: say why, not what.
- No exceptions across OBS callback boundaries. Catch at the edge and log.
- Log with `obs_log(LOG_INFO|LOG_WARNING|LOG_ERROR, ...)` from the template, which
  prefixes `[clip-atlas]`.
- Every user-facing string goes through `obs_module_text("Key")`, with the key added to
  `data/locale/en-US.ini`.

## Correctness rules
- **Strings are UTF-8.** Convert to a path only with `clip_atlas::path_from_utf8`, never
  with `std::filesystem::path(std::string)`, because on Windows that goes through the
  ANSI code page.
- **Threading:**
  - OBS source signals can arrive on any thread. Hop to the UI thread with
    `run_on_ui(...)` before touching plugin state or calling `obs_frontend_*`.
  - Never do file I/O on the UI thread. Use `file-mover`.
- **OBS references:**
  - Release every `obs_source_t`, `obs_data_t` and `obs_output_t` you acquire.
  - Keep `obs_weak_source_t` for long-lived references.
  - Disconnect every signal on unload.
- **Never delete or overwrite a user's file** unless a verified copy exists.
- No new third-party dependencies. Allowed: libobs, obs-frontend-api, Qt 6 Widgets, the
  Win32 SDK, the C++ standard library, and Catch2 (tests only).
- Never guess an OBS API, signal name or calldata field. Use only what your brief or the
  OBS headers show. If you're unsure, say so in your hand-over note.

## Commands
```sh
# unit tests (Linux or Windows)
cmake -S tests -B build/tests && cmake --build build/tests && ctest --test-dir build/tests --output-on-failure
# format check
clang-format --dry-run --Werror <files>
```

## Git
Workers never commit or push. Only the orchestrator does, with the message
`WP<ID>: <title>` and the session's attribution trailer.
