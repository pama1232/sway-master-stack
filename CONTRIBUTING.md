# Contributing to sway-mstack

[Русская версия](CONTRIBUTING_ru.md)

Thanks for your interest in the project. This page describes the minimum process
that keeps the layout and compatibility intact.

## Scope

`sway-mstack` implements one layout well: master/stack for Sway over the
standard IPC. Changes that add compositor patches, new keybindings or a second
layout are out of scope unless discussed in an issue first.

## Before you start

- Open an issue for large changes. The daemon is small but has many invariants;
  an obvious refactor can break zoom or closing the master.
- One pull request, one logical change. Keep infrastructure, tests and behavior
  in separate commits.
- Do not add demo media (GIF/MP4) to the repository. The demo lives outside the
  repo and README links to it.

## Build

```sh
CC=clang cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# For logic work, use Debug with ASan/UBSan:
CC=clang cmake -B build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug -j
```

Requirements: CMake >= 3.25, clang >= 19 (C23), lld, json-c, pkgconf. For a
portable binary, add `-DSMS_NATIVE=OFF`.

## Source layout

The daemon is one translation unit: `src/main.c` includes `src/sms.h` and the
fragments in `src/parts/*.inc` in declaration order. CMake compiles only
`src/main.c`.

- Put new code in the matching fragment under `src/parts/`.
- If you need a new fragment, include it in `src/main.c` after its dependencies
  and update the table in `ARCHITECTURE_ru.md` and `ARCHITECTURE_en.md`.
- Do not compile `.inc` files separately or turn them into standalone `.c` files
  without discussion. Unit tests include `src/main.c` and check static helpers.
- The split is for navigation. It must not change the preprocessed token stream
  or code generation. Verify with a Release build.

## Code style

- **C23**, no GNU extensions (`CMAKE_C_EXTENSIONS OFF`).
- Two spaces per indent, 80 columns.
- Use `nullptr`, `bool`, `constexpr` and `[[nodiscard]]` as in the existing
  code.
- **Comments in `src/` are in Russian.** Do not translate them to English or
  remove invariant explanations. They record measured Sway behavior.
- Mark new static functions `[[nodiscard]]` when ignoring the result is a bug.
- Do not add dependencies without a reason. The project uses `json-c` and POSIX
  only.
- Compiler warnings (`-Wall -Wextra -Wpedantic -Wconversion -Wshadow`) must stay
  clean.

## Tests

Before a pull request:

```sh
# Unit tests (Release and Debug)
ctest --test-dir build --output-on-failure
ctest --test-dir build-debug --output-on-failure

# E2E in a private headless Sway session
tests/e2e/run.sh
```

If you change layout behavior, add a scenario to `tests/e2e/run.sh` and, where
possible, a check to `tests/unit/test_main.c`. The test includes `src/main.c`,
so new `src/parts/*.inc` fragments are picked up automatically. See
[tests/README.md](tests/README.md) for the harness and debugging tips.

E2E must not touch the live Sway session. Every scenario must start its own
`XDG_RUNTIME_DIR`, `SWAYSOCK` and `WAYLAND_DISPLAY`.

## Commits

- Use a short imperative subject with a prefix: `fix:`, `feat:`, `docs:`,
  `test:`, `build:`, `ci:`, `refactor:`.
- Explain **why** in the body, not only what. For fixes, include the
  reproduction and the measured Sway behavior.
- Record user-facing changes in `CHANGELOG.md` under `[Unreleased]`. Do not bump
  the version or release date without agreement.

Example:

```text
fix: keep focus on the workspace when the last master dies

Sway moves focus to another output when the final window of a
workspace disappears. Guard the focus path with g_focus_ws so the
user stays on the emptied workspace.
```

## Pull request

- [ ] Release and Debug builds pass without warnings.
- [ ] `ctest` (unit) is green in both configurations.
- [ ] `tests/e2e/run.sh` is green; new scenarios are added when behavior
  changes.
- [ ] `CHANGELOG.md` is updated for user-facing changes.
- [ ] Comments in `src/` are still in Russian.
- [ ] New `src/parts/*.inc` fragments are included in `src/main.c`.
- [ ] The diff has no demo media, build directories or personal files.

## Documentation

- `README.md`: installation, usage, features and source layout.
- `ARCHITECTURE_ru.md` and `ARCHITECTURE_en.md`: internal design. When you
  change the event pipeline, fast paths, state or `src/` layout, update both
  versions.
- `tests/README.md`: test harness.
- `CHANGELOG.md`: user-facing history.

## Git history

The repository history has already been cleaned of heavy demo files. Do not
rewrite history or force-push without explicit agreement. It breaks clones and
tags for other contributors.
