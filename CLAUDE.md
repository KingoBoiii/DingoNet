# DingoNet — Claude Context

## What this project is

DingoNet is a cross-platform C++20 netcode library for DingoEngine games: byte serialization, transports (TCP, loopback, a network conditioner, Steam as a source drop) and a configurable session layer (handshake, seats, ping, timeouts). It is extracted from *Headstone*'s online co-op (`Headstone/src/Net/`) and becomes an optional engine module at v1.0. The core library (`DingoNet`) is engine-free: no DingoEngine header, no glm, and it logs through a handler the game installs. A thin `DingoNet-Engine` library (a `NetLayer`, UUID-keyed scene replication) arrives in v0.4 and is the only part that links the engine. Games consume a prebuilt package, laid out like the engine's: release zips per configuration, unpacked to `vendor/DingoNet/{include, lib/<Config>, VERSION.txt}`.

## Build system

- **Tool**: Premake5 5.0.0-beta8, committed in `vendor/premake/bin/` (`premake5.exe` and the Linux `premake5`).
- **Scripts**: `scripts/windows/*.bat` and `scripts/linux/*.sh`, the same four on each platform (`Generate`, `Build`, `Test`, `Package`), runnable from any directory. A configuration argument is optional (`Debug` by default; `Release`, `Distribution`; Linux also takes lower case). Script internals: `scripts/README.md`.
- **Warnings**: `warnings "Extra"` + `fatalwarnings` on both platforms (`/W4 /WX`, `-Wall -Wextra -Werror`). A build with a warning is a failed build.
- **Configs**: `Debug` (`DN_DEBUG`), `Release` (`DN_RELEASE`, symbols on), `Distribution` (`DN_DIST`, no symbols). `DN_PLATFORM_WINDOWS` / `DN_PLATFORM_LINUX`.
- **Output**: `build/bin/<Config>-<system>-x86_64/<Project>/`, intermediates in `build/bin-int/`. `DingoNet` is a static lib, `DingoNet-Tests` a console app (its project files land in `tests/`, git-ignored).
- **CI** (`.github/workflows/`): `build.yml` builds and tests Debug, Release and Distribution on `windows-latest` and `ubuntu-latest` on every push and PR to master. `release.yml` packages all three configs on both OSes when a release with a `vX.Y.Z` tag is published; the Windows runners lack v145, so CI builds with `/p:PlatformToolset=v143`. Details: `scripts/README.md`.
- **Version**: `VERSION` holds `major.minor`; CI appends the commit count and overwrites `include/DingoNet/BuildInfo.h` (`DN_VERSION_*`, read by `Dingo::Net::GetVersion()`). The checked-in `BuildInfo.h` is a `0.1.0` placeholder.

## Project structure

```
include/DingoNet/        Public API headers: plain C++20, no platform header
src/                     Implementations
  Internal/              Logging helpers (Internal::LogInfo...), the unreliable size check
  Platform/              Socket.h (internal socket API, one SocketError enum), Socket.cpp,
                         WinSocket.cpp (Winsock2) | PosixSocket.cpp (BSD sockets): the only place
                         for winsock2.h, sys/socket.h, unistd.h; premake builds one per system
tests/                   DingoNet-Tests: own runner (TestFramework.h), one file per area
  steam/                 DingoNet-SteamTests (only with --steam-sdk): SteamTransport on a fake Steam
steam/                   SteamTransport source drop, shipped in the package
docs/                    API details per area: messages, transports, session, mirror, steam, tests
scripts/windows/         Generate.bat, Build.bat, Test.bat, Package.bat
scripts/linux/           Generate.sh, Build.sh, Test.sh, Package.sh
vendor/premake/          Premake binaries — NEVER modify vendor code
.claude/plans/           Plans (2026-10-12-dingonet-plan.md is the v0.1 → v1.0 plan)
CHANGELOG.md             Released versions with dates, builds and review notes
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
- **Messages and unreliable size cap** (`Messages.h`): typed game messages, a router and per-id stats; unreliable packets are capped at 1100 bytes. Details: `docs/messages.md`, read it before changing messages.
- **Transports** (`Transport.h`): TCP, loopback and the conditioner behind one `Transport` interface. Details: `docs/transports.md`, read it before changing a transport.
- **Session** (`Session.h`): handshake, seats, events, pings, timeouts and epochs over a `Transport`. Details: `docs/session.md`, read it before changing the session or its wire.
- **Mirror** (`Mirror.h`): header-only interpolation, extrapolation and render clock for replicated state. Details: `docs/mirror.md`, read it before changing mirror code.

## Tests

`DingoNet-Tests` uses its own runner (`tests/TestFramework.h`): `TEST(name)` registers a case, `CHECK(cond)` records a failure with file and line, and `main` prints `PASS`/`FAIL` per case and a summary, exiting non-zero on any failure. The whole run stays around 3 s on Windows and under 1 s on Linux; a test that slows it is a bug. Rigs, per-file coverage and the cross-OS check (`--tcp-host`/`--tcp-client`): `docs/tests.md`.

## Third-party vendors

| Vendor | Role |
|---|---|
| premake | Build generation (binaries only) |

The core links only the OS (Winsock on Windows, libc on Linux). Steam ships as source (`steam/`) that the game compiles against its own Steamworks SDK. Details: `docs/steam.md`, read it before changing the Steam transport.

## Docs & reviews

- `docs/` — API details per area (`messages`, `transports`, `session`, `mirror`, `steam`, `tests`); read the one for the area you change.
- `.claude/plans/` — dated plans; a plan's "As built" notes override its design.
- `.claude/reviews/` — dated code-review reports, findings keyed with a per-finding fix status. Netcode diffs get an Opus review before a tag.
- `CHANGELOG.md` — released versions with dates, builds and review notes.
- `README.md` — what it is, platforms, build commands. From v0.4 it keeps the table of DingoNet version → minimum engine version.

## Roadmap

See `.claude/plans/2026-10-12-dingonet-plan.md` ("Versions").

- **v0.1.0 Extract**: the library extracted from Headstone.
- **v0.1.1**: the v0.1 review fixes, wire-compatible with v0.1.0.
- **v0.2.0 Structure**: message registry, session epochs, `Mirror.h`; reviews fixed.
- **v0.3 UDP**: a reliable-UDP transport (ENet proposed), LAN play without Steam.
- **v0.4 Engine bridge**: `DingoNet-Engine`. **v1.0 Module**: an optional DingoEngine module, listed under the engine roadmap's Modules.

## Workflow

Do not commit unless asked. One commit per phase of the plan, after its review. Run one session per phase: use the session-handoff skill, then start a new session. A session should not span more than one day.

## Sub-agents

- **Model and effort for this repo** (overrides the global table, whose "Opus for netcode" rule would otherwise cover this whole repo): phase implementation, diff reviews ("Review ... diff"), docs, scripts and tests run on Sonnet at medium. Opus at high only for wire or transport design, the pre-tag netcode review, and root-cause work with an unknown cause (transport, session, wire); say why in the spawn description. Fable never, unless the user asks.
- **The session itself**: the global `~/.claude/settings.json` defaults the session to Sonnet at medium. Switch the session to Opus/high only for the three Opus cases above, and back afterwards.
- **`fork`** inherits the session's model: when the session runs on Opus or Fable, spawn a fresh agent with `model` set instead.
- **Verification** (build, tests) goes to Haiku at low, reporting failures only.
