# DingoNet — Claude Context

## What this project is

DingoNet is a cross-platform C++20 netcode library for DingoEngine games: byte serialization, transports (TCP, loopback, a network conditioner, Steam as a source drop) and a configurable session layer (handshake, seats, ping, timeouts). It is extracted from *Headstone*'s online co-op (`Headstone/src/Net/`) and becomes an optional engine module at v1.0. The core library (`DingoNet`) is engine-free: no DingoEngine header, no glm, and it logs through a handler the game installs. A thin `DingoNet-Engine` library (a `NetLayer`, UUID-keyed scene replication) arrives in v0.4 and is the only part that links the engine. Games consume a prebuilt package, laid out like the engine's: release zips per configuration, unpacked to `vendor/DingoNet/{include, lib/<Config>, VERSION.txt}`.

## Build system

- **Tool**: Premake5 5.0.0-beta8, committed in `vendor/premake/bin/` (`premake5.exe` and the Linux `premake5`).
- **Scripts**: `scripts/windows/*.bat` and `scripts/linux/*.sh`, the same three on each platform, runnable from any directory. A configuration argument is optional (`Debug` by default; `Release`, `Distribution`; Linux also takes lower case).
  - `Generate`: premake (`vs2026` on Windows, `gmake` on Linux); extra arguments pass through to premake.
  - `Build`: generates first if the solution or Makefile is missing, then MSBuild on `DingoNet.slnx` (found through `vswhere`) or `make -j"$(nproc)"`.
  - `Test`: `Build`, then runs `DingoNet-Tests`; the exit code is the runner's.
  - `Package [Config...]` (default all three): builds each and writes `build/package/DingoNet-<DN_VERSION>.<build>-<Config>-<os>-x86_64.zip` with `include/`, `lib/<Config>/{DingoNet.lib | libDingoNet.a}`, `steam/`, `LICENSE.txt`, `VERSION.txt`; no top-level folder, forward-slash entries (Windows zips through `tar.exe`, Linux through `zip` or `python3 -m zipfile`). `DN_VERSION` (MAJOR.MINOR.PATCH, default `<VERSION>.0`), `DN_BUILD_NUMBER` (default the commit count), `DN_NO_BUILD=1` (`.bat` only: skip building, for CI). It stamps `BuildInfo.h` for the build and restores it with a fresh timestamp, so later builds recompile `Version.cpp`.
  - The `.bat` files pause only when launched from Explorer (`%CMDCMDLINE%` holds their full path), so shells and CI never hang on them.
- **CI** (`.github/workflows/`): `build.yml` builds and tests Debug, Release and Distribution on `windows-latest` and `ubuntu-latest` on every push and PR to master. `release.yml` runs on a `v*` tag or a manual dispatch with a `tag` input, checks the tag against `VERSION`, packages all three configs on both OSes and uploads them with `gh release upload --clobber`, creating the release if it is missing. Windows CI builds with `/p:PlatformToolset=v143` (`WINDOWS_TOOLSET`), as the engine's does, since the runners lack v145; a v143 static lib links into a v145 game.
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
  Internal/              Logging helpers (Internal::LogInfo...), the unreliable size check
  Platform/              Socket.h (internal socket API, one SocketError enum), Socket.cpp,
                         WinSocket.cpp (Winsock2) | PosixSocket.cpp (BSD sockets): the only place
                         for winsock2.h, sys/socket.h, unistd.h; premake builds one per system
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
- **Transports** (`Transport.h`): `Send(PeerId, std::span<const uint8_t>, Delivery)`, `Poll(packets, events)` once a frame (flushes, then appends arrivals and `PeerEvent`s), `Disconnect`, `Close`, `Describe`, public `BytesIn`/`BytesOut`. A disconnect is a `DisconnectReason` (`Closed`, `TimedOut`, `Refused`, `Reset`, `ConnectFailed`, `Error`) plus a `Detail` string for logs only; a peer we `Disconnect` raises no event on our side, and what we sent before it still arrives. `TcpTransport::Listen/Connect(address, port, error, TcpParams)` (IPv4; port 0 picks one, `GetPort()`; caps 64 KB frame, 4 MB send queue checked after each flush, 16 live connections (lingering ones don't count), 256 KB read per connection per `Poll` so a flooding peer can't stall a frame, 5 s connect, 2 s close linger): frames are a 4-byte little-endian length + payload, wire-compatible with Headstone; a reliable packet over the frame cap is logged and dropped at the sender, a bigger incoming frame or a full send queue drops the peer (`Error`). A connect that fails at once still reports through `Poll`. **Read before send** in `Poll` on both OSes (a send to a peer that has closed brings back a reset, and Windows then discards unread data, a parting `Bye` with it); `Disconnect` keeps flushing the whole queue in later `Poll`s, shuts our side only once it is empty, then drains until their FIN (all within `CloseLinger`); `Close` flushes for at most 0.25 s (`WaitWritable`), then hangs up. Every socket call goes through `src/Platform/Socket.h`; sockets are never inherited by child processes (`SOCK_CLOEXEC`/`accept4` on Linux, `FD_CLOEXEC` elsewhere, `WSA_FLAG_NO_HANDLE_INHERIT` + `SetHandleInformation` on Windows); Linux sends use `MSG_NOSIGNAL` (`SO_NOSIGPIPE` where it exists), `EINTR` is retried, `WinSocket.cpp` refcounts `WSAStartup` under a mutex and carries `#pragma comment(lib, "ws2_32.lib")`, so a game linking the static lib names nothing extra. Non-blocking connects finish through `PollConnect` (`select` except-set on Windows, `poll` on POSIX, then `SO_ERROR`). `LoopbackTransport::Listen()`/`Connect(host)` and `CreateLoopbackPair()`: in-process queues, many clients per host, delivered at the receiver's next `Poll`, never lost. `Conditioner(std::unique_ptr<Transport>, ConditionerParams)` wraps any transport: lag, jitter (reliable packets never overtake each other), loss of unreliable ones only, its own `mt19937` seeded by `Seed` (99), and an optional `Clock` hook (tests and a game's net clock); `Disconnect`/`Close` send what it holds first, and what it holds for a peer the inner transport reports lost is dropped (SteamIDs come back); `Dropped` counts losses.
- **Session** (`Session.h`, `src/Session.cpp`, wire in `src/SessionMessages.{h,cpp}`): owns a `Transport`; `StartHost`/`StartClient(transport, name)`, `Update()` once a frame (time from `steady_clock`, or `SessionParams::Clock`), `TakeEvents`/`TakeMessages`, `Stop` (a `Bye` to everyone, no event; the destructor calls it), host `Kick(slot)` and `SetAccepting`. Slot 0 is the host; `SessionParams::MaxSlots` (1–32, clamped with a warning) sizes the seats; a client adopts a host's smaller count from the `Welcome` and turns a larger one away (`Rejected(ProtocolMismatch)`, then a `Bye`), so no event ever names a slot past the game's own arrays. The first byte is the message id: games own `0x00–0xEF`, the session `0xF0–0xFF` (`Hello` F0, `Welcome` F1, `Reject` F2, `Ping` F3, `Pong` F4, `Bye` F5, `SeatJoined` F6, `SeatLeft` F7); `Send`/`Broadcast` of a reserved id logs an error and drops it, and an empty packet is the game's. `Hello` = `k_SessionProtocol` (1; bump it whenever these messages change) + the game's `Protocol` + `ContentHash` + name + `Build`; the host checks session protocol, then `Protocol` (both `ProtocolMismatch`), `ContentHash` (`ContentMismatch`), `SetAccepting` (`NotAccepting`), a free seat (`Full`). Hello's leading session protocol and the whole `Reject` (reason, the host's versions and build) never change layout, so two session protocols can still turn each other away. Events: `Joined`/`Left` on the host and, through `Welcome`'s roster and `SeatJoined`/`SeatLeft`, on every client for the other clients (so `SlotTaken`/`NameOf` are right everywhere; roster joins carry `Existing`, so a game doesn't announce players already there); `Welcomed` (Name = the host's), `Rejected` (client: the session stopped; host: who it turned away), `Lost` (client: stopped; `Slot` -1 if never seated). Codes: `RejectReason { ProtocolMismatch, ContentMismatch, NotAccepting, Full }`, `LeaveReason { Bye, TimedOut, Kicked, TransportLost }` (with `TransportLost`, `Event::Transport` holds the transport's `DisconnectReason`), plus a log-only `Detail` (the transport's words, or both sides' versions on a reject, a peer's build cut to 64 printable ASCII). A slot's queued game messages are dropped when it goes, so none is read after its `Left` or as the next occupant's. Ping every `PingInterval` (1 s, clamped to `Timeout / 2`; unreliable, u32 ms since start echoed back) → `RttMs(slot)`, smoothed 0.3 from the first sample (a client pings only the host, so only its slot 0 has one); a seated peer silent for `Timeout` (5 s) is dropped, a connection without a `Hello`/`Welcome` after `HandshakeTimeout` (5 s; a client's runs from `StartClient` and restarts at the transport's `Connected`) too. `NameFilter` (null = `DefaultNameFilter`: printable ASCII, trimmed, 16 chars, "Player" if empty) runs on every name the session takes in, its own included. Every decode is strict (short read, trailing bytes, out-of-range enum or slot = malformed): malformed, unexpected-side and unknown session packets are dropped, never fatal, with one warning per peer, problem and id (the rest counted); a host hangs up on a peer after 32 (`Bye(Kicked)`). Game packets from an unseated peer go to the host's `SessionParams::UnseatedPacket` hook, which may return a raw reply that is sent before the hang-up (how a game answers an older client's Hello; without it that client only times out), else are dropped. Order in `Update`: the transport's events in order, except that a plain disconnect waits for the packets (a `Bye` that arrives with the hang-up reads as `Bye`) and a disconnect followed by a connect of the same id (a SteamID back) drops the old peer first; then packets (one from an untracked peer makes the host `Disconnect` it), deferred disconnects, pings, timeouts.

## Tests

`DingoNet-Tests` uses its own runner (`tests/TestFramework.h`): `TEST(name)` registers a case, `CHECK(cond)` records a failure with file and line, and `main` prints `PASS`/`FAIL` per case and a summary, exiting non-zero on any failure. Network tests run over the loopback transport (a `Conditioner` with a fake clock for lag), or over real sockets on `127.0.0.1` port 0, each poll loop capped by time (`TransportTestUtils.h`: `PumpUntil`, `LogCapture`); session tests (`tests/SessionTests.cpp`) drive time through `SessionParams::Clock` and pump a fixed number of rounds, with one handshake over TCP. The whole run stays under ~3 s (2.6 s on Windows, where a refused connect waits out SYN retries; 0.1 s on Linux). A cross-OS check closes each transport phase: `DingoNet-Tests --tcp-host=<ip>:<port>` on one machine, `--tcp-client=<ip>:<port>` on the other (`tests/NetCheck.cpp`, 200 echoed packets). From WSL2 in NAT mode a Windows host needs an inbound firewall rule on the `vEthernet (WSL (Hyper-V firewall))` interface (or mirrored networking); the reverse, a WSL2 host on `0.0.0.0` and a Windows client on `127.0.0.1`, needs nothing.

## Third-party vendors

| Vendor | Role |
|---|---|
| premake | Build generation (binaries only) |

The core links only the OS: Winsock (`ws2_32`, pulled in by `#pragma comment` in `WinSocket.cpp`) on Windows, libc on Linux. Steam ships as source (`steam/`) that the game compiles against its own Steamworks SDK: `SteamTransport::Create(ISteamNetworkingMessages*, host, allowed, log, isAlive)` on channel 0, with the stale-queue drain on create; `log` is a callback because the package exposes no log function, and `isAlive` (or destroying it before `SteamAPI_Shutdown()`) keeps `Close` off a dead interface. Remote, local and connectivity failures are `ConnectFailed` (Steam has no explicit deny, so nothing is `Refused`), a disconnected peer's messages are dropped for 3 s, and failed sends are logged. v0.3 adds a reliable-UDP library (ENet proposed).

## Docs & reviews

- `.claude/plans/` — dated plans; a plan's "As built" notes override its design.
- `.claude/reviews/` — dated code-review reports, findings keyed with a per-finding fix status. Netcode diffs get an Opus review before a tag.
- `README.md` — what it is, platforms, build commands. From v0.4 it keeps the table of DingoNet version → minimum engine version.

## Roadmap

See `.claude/plans/2026-10-12-dingonet-plan.md` ("Versions").

- **v0.1 Extract** (in progress): phase 1 (repo skeleton, scripts), phase 2 (bytes), phase 3 (transports: Winsock + POSIX TCP, loopback, conditioner), phase 4 (session) and phase 5 (package + CI) done; next phase 6 (Headstone running on the package).
- **v0.2 Structure**: message registry, session epoch, generic `Mirror<T>` interpolation.
- **v0.3 UDP**: a reliable-UDP transport, LAN play without Steam.
- **v0.4 Engine bridge**: `DingoNet-Engine`.
- **v1.0 Module**: an optional DingoEngine module, listed under the engine roadmap's Modules.

## Workflow

Do not commit unless asked. One commit per phase of the plan, after its review.
