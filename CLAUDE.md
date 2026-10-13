# DingoNet — Claude Context

## What this project is

DingoNet is a cross-platform C++20 netcode library for DingoEngine games: byte serialization, transports (TCP, loopback, a network conditioner, Steam as a source drop) and a configurable session layer (handshake, seats, ping, timeouts). It is extracted from *Headstone*'s online co-op (`Headstone/src/Net/`) and becomes an optional engine module at v1.0. The core library (`DingoNet`) is engine-free: no DingoEngine header, no glm, and it logs through a handler the game installs. A thin `DingoNet-Engine` library (a `NetLayer`, UUID-keyed scene replication) arrives in v0.4 and is the only part that links the engine. Games consume a prebuilt package, laid out like the engine's: release zips per configuration, unpacked to `vendor/DingoNet/{include, lib/<Config>, VERSION.txt}`.

## Build system

- **Tool**: Premake5 5.0.0-beta8, committed in `vendor/premake/bin/` (`premake5.exe` and the Linux `premake5`).
- **Scripts**: `scripts/windows/*.bat` and `scripts/linux/*.sh`, the same three on each platform, runnable from any directory. A configuration argument is optional (`Debug` by default; `Release`, `Distribution`; Linux also takes lower case).
  - `Generate`: premake (`vs2026` on Windows, `gmake` on Linux); extra arguments pass through to premake.
  - `Build`: generates first if the solution or Makefile is missing, then MSBuild on `DingoNet.slnx` (found through `vswhere`) or `make -j"$(nproc)"`.
  - `Test`: `Build`, then runs `DingoNet-Tests`; the exit code is the runner's.
  - The `.bat` files pause only when launched from Explorer (`%CMDCMDLINE%` holds their full path), so shells and CI never hang on them.
- **Windows** (MSVC, toolset v145): `staticruntime "Off"` and v145 match Headstone's, so a game links the engine and DingoNet packages with one CRT. On LNK1318, build with `/m:1`.
- **Linux** (GCC >= 13, x86-64): tested locally in WSL2 (`Ubuntu-24.04`, repo at `/mnt/d/Git/Github/KingoBoiii/DingoNet`). `/mnt/d` is case-insensitive, so a wrong-cased include still builds there; CI on `ubuntu-latest` catches it.
- **Warnings**: `warnings "Extra"` + `fatalwarnings` on both platforms (`/W4 /WX`, `-Wall -Wextra -Werror`). A build with a warning is a failed build.
- **Configs**: `Debug` (`DN_DEBUG`), `Release` (`DN_RELEASE`, symbols on), `Distribution` (`DN_DIST`, no symbols). `DN_PLATFORM_WINDOWS` / `DN_PLATFORM_LINUX`.
- **Output**: `build/bin/<Config>-<system>-x86_64/<Project>/`, intermediates in `build/bin-int/`. `DingoNet` is a static lib, `DingoNet-Tests` a console app (its project files land in `tests/`, git-ignored).
- **Version**: `VERSION` holds `major.minor`; CI appends the commit count and overwrites `include/DingoNet/BuildInfo.h` (`DN_VERSION_*`, read by `Dingo::Net::GetVersion()`). The checked-in `BuildInfo.h` is a `0.1.0` placeholder.

## Project structure

```
include/DingoNet/        Public API headers: plain C++20, no platform header
src/                     Implementations
  Internal/              Logging helpers (Internal::LogInfo...)
  Platform/              The only place for winsock2.h, sys/socket.h, unistd.h (phase 3)
tests/                   DingoNet-Tests: own runner (TestFramework.h), one file per area
scripts/windows/         Generate.bat, Build.bat, Test.bat
scripts/linux/           Generate.sh, Build.sh, Test.sh
vendor/premake/          Premake binaries — NEVER modify vendor code
.claude/plans/           Plans (2026-10-12-dingonet-plan.md is the v0.1 → v1.0 plan)
```

## Cross-platform (hard requirement)

Windows and Linux from v0.1, not later. Every phase builds and passes its tests on both before it is committed.

- Platform headers (`winsock2.h`, `sys/socket.h`, `unistd.h`) only under `src/Platform/`; public headers are plain C++20.
- Time from `std::chrono::steady_clock`, never `QueryPerformanceCounter` or `clock_gettime` directly.
- Socket errors map to one `SocketError` enum (`WSAEWOULDBLOCK` and `EAGAIN`/`EWOULDBLOCK` are both `WouldBlock`). Linux sends use `MSG_NOSIGNAL`: a send to a closed peer otherwise raises `SIGPIPE` and kills the game.
- macOS is not a target (the engine doesn't build there), but nothing may rule it out.

## Code conventions

| Thing | Convention |
|---|---|
| Namespace | `Dingo::Net` (internals in `Dingo::Net::Internal`) |
| Members | `m_Name` (instance), `s_Name` (static), `UPPER_SNAKE_CASE` constants/macros |
| Macros | `DN_` prefix |
| Formatting | tabs, braces on new lines |
| Log calls | `Internal::LogTrace/LogInfo/LogWarn/LogError` with `std::format` arguments |

- **`<Windows.h>` macros**: a game's headers may pull in `<Windows.h>` without `NOMINMAX`. Write `(std::min)(a, b)` / `(std::max)(...)` / `(std::numeric_limits<T>::max)()` in public headers, and never name anything `near` or `far`.
- In a public header, a member named after its own type spells the type qualified: `Dingo::Net::Version Version`. MSVC accepts `Version Version`, but GCC rejects it (`-Wchanges-meaning`) in every translation unit that includes the header.
- Scripts: `.sh` files are LF and executable in git (`git update-index --chmod=+x`), `.bat` files CRLF (`.gitattributes` enforces both). Git Bash `sed -i` turns CRLF files into LF; restore with `perl -pi -e 's/(?<!\r)\n/\r\n/'`.

## Comments — keep them to a minimum

Do not write code comments unless absolutely necessary. A comment must earn its place by stating a load-bearing "why" — a constraint, invariant, or non-obvious consequence the code cannot express itself. Never narrate what the next line does, never leave change-tracking or review commentary ("added X", "fixed Y"), and apply the same restraint to scripts, YAML and tests. When in doubt, leave it out.

## Key patterns

- **Logging**: `SetLogHandler(LogHandler)` (`Log.h`); `nullptr` restores the default, which writes `[DingoNet] <LEVEL>: <msg>` to stderr. The handler lives in a `std::atomic`. A game routes it to its own log (DingoEngine: `DE_INFO_TAG` etc.). The library never prints any other way.
- **No threads**: transports and sessions are pumped once a frame by the game (Headstone's model). Nothing in the core starts a thread.
- **Library version ≠ wire version**: the session handshake carries `k_SessionProtocol` (bumped only when DingoNet's own session messages change) beside the game's protocol number and a game-supplied content hash. A patch release never changes the wire.
- **Reason codes, not strings**: disconnect and reject reasons are enums; the game owns the text players read.
- **Bytes** (`Bytes.h`): `ByteWriter`/`ByteReader`, little-endian, wire-identical to Headstone's (a golden-bytes test pins it; never change an encoding without a protocol bump). Quantizers: `Coord` (centimetres in an `int16`, ±327 m), `Angle16`/`Angle8` (wrapped to [0, 360)), `Unit8` (0–1); NaN encodes as 0. A reader fails for good on the first short read and returns zeros from then on: check `Ok()` once after decoding. It holds a span, so the bytes must outlive it.
- **Unreliable size cap**: `MAX_UNRELIABLE_BYTES = 1100` (Steam drops a whole fragmented unreliable message); `Send` logs an error and drops a larger one.

## Tests

`DingoNet-Tests` uses its own runner (`tests/TestFramework.h`): `TEST(name)` registers a case, `CHECK(cond)` records a failure with file and line, and `main` prints `PASS`/`FAIL` per case and a summary, exiting non-zero on any failure. Network tests run over the loopback transport, or over real sockets on `127.0.0.1`; a cross-OS check (a Windows host, a WSL2 client) closes each transport phase.

## Third-party vendors

| Vendor | Role |
|---|---|
| premake | Build generation (binaries only) |

The core links only the OS: Winsock (`ws2_32`) on Windows from phase 3. Steam ships as source (`steam/`) that the game compiles against its own Steamworks SDK. v0.3 adds a reliable-UDP library (ENet proposed).

## Docs & reviews

- `.claude/plans/` — dated plans; a plan's "As built" notes override its design.
- `.claude/reviews/` — dated code-review reports, findings keyed with a per-finding fix status. Netcode diffs get an Opus review before a tag.
- `README.md` — what it is, platforms, build commands. From v0.4 it keeps the table of DingoNet version → minimum engine version.

## Roadmap

See `.claude/plans/2026-10-12-dingonet-plan.md` ("Versions").

- **v0.1 Extract** (in progress): phase 1 (repo skeleton, scripts) and phase 2 (bytes) done; next phase 3 (transports: Winsock + POSIX TCP, loopback, conditioner), then session, package + CI, and Headstone running on the package.
- **v0.2 Structure**: message registry, session epoch, generic `Mirror<T>` interpolation.
- **v0.3 UDP**: a reliable-UDP transport, LAN play without Steam.
- **v0.4 Engine bridge**: `DingoNet-Engine`.
- **v1.0 Module**: an optional DingoEngine module, listed under the engine roadmap's Modules.

## Workflow

Do not commit unless asked. One commit per phase of the plan, after its review.
