# DingoNet — Claude Context

## What this project is

DingoNet is a cross-platform C++20 netcode library, extracted from the Headstone co-op game and meant to become an optional DingoEngine module. The core (`DingoNet`) is engine-free: no DingoEngine header, no glm. It logs through a handler the game installs. Plan: `.claude/plans/2026-10-12-dingonet-plan.md`.

## Build

- **Windows** (MSVC v145): `./vendor/premake/bin/premake5.exe vs2026`, then MSBuild on `DingoNet.slnx` (`/p:Configuration=Debug /p:Platform=x64 /m`; on LNK1318 use `/m:1`). Do not run `Generate-Windows.bat` from a non-interactive shell (it ends in `PAUSE`).
- **Linux** (GCC >= 13, x86-64): `./Generate-Linux.sh`, then `make -j"$(nproc)" config=debug`.
- Configs: `Debug`, `Release`, `Distribution`. Outputs: `build/bin/<Config>-<system>-x86_64/<Project>/`.
- Tests: `DingoNet-Tests` (own runner in `tests/TestFramework.h`: `TEST(name)`, `CHECK(cond)`); exits non-zero on failure.

## Cross-platform (hard requirement)

Windows and Linux from v0.1. Every phase builds and passes its tests on both.

- Platform headers (`winsock2.h`, `sys/socket.h`, `unistd.h`) only under `src/Platform/`; public headers are plain C++20.
- Time from `std::chrono::steady_clock`, never `QueryPerformanceCounter` or `clock_gettime` directly.
- Socket errors map to one `SocketError` enum (`WSAEWOULDBLOCK` and `EAGAIN`/`EWOULDBLOCK` are both `WouldBlock`). Linux sends use `MSG_NOSIGNAL`, since a send to a closed peer otherwise raises `SIGPIPE`.
- A member named after its type spells the type qualified (GCC rejects it otherwise).
- macOS is not a target, but nothing may rule it out.

## Code conventions

| Thing | Convention |
|---|---|
| Namespace | `Dingo::Net` (internals in `Dingo::Net::Internal`) |
| Members | `m_Name` (instance), `s_Name` (static), `UPPER_SNAKE_CASE` constants/macros |
| Macros | `DN_` prefix |
| Formatting | tabs, braces on new lines |

- Public headers live in `include/DingoNet/`, are plain C++20, and include no platform header. Internal headers live in `src/` (`src/Internal/` for logging).
- Write `(std::min)(a, b)` / `(std::max)(...)` / `(std::numeric_limits<T>::max)()` in headers, since `<Windows.h>` defines `min`/`max`, and never name anything `near` or `far`.
- A member named after its own type is written qualified: `Dingo::Net::Version Version`, not `Version Version`.
- Internal logging: `Internal::LogInfo/LogWarn/LogError/LogTrace` with `std::format` arguments.

## Comments — keep them to a minimum

Do not write code comments unless absolutely necessary. A comment must earn its place by stating a load-bearing "why": a constraint, invariant, or non-obvious consequence the code cannot express itself. Never narrate what the next line does, never leave change-tracking or review commentary, and apply the same restraint to scripts, YAML and tests. When in doubt, leave it out.

## Versioning

- `VERSION` holds `major.minor`; CI appends the commit count as the build number and writes `include/DingoNet/BuildInfo.h` (`DN_VERSION_*`, read by `Dingo::Net::GetVersion()`). The checked-in `BuildInfo.h` is a `0.1.0` placeholder.
- Tags `vX.Y.Z` on release branches, a GitHub release per tag with the package zips.
- The library version is not the wire version: the session layer's handshake carries `k_SessionProtocol` (bumped only when DingoNet's session messages change), beside the game's protocol number and content hash. A patch release never changes the wire.
- The core has no engine compatibility. From v0.4, `DingoNet-Engine` builds against a pinned engine package, and the README keeps a table of DingoNet version to minimum engine version.

## Workflow

Do not commit unless asked. One commit per phase of the plan. Vendor code (`vendor/premake`) is never modified.
