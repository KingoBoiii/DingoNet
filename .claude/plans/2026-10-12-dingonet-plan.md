# DingoNet — a netcode library for DingoEngine games

The roadmap's **Networking & multiplayer** module (`ROADMAP.md:419-426`) starts life as a standalone
library, **DingoNet**, that games link. It is extracted from *Headstone*'s co-op, which already has a
working, soak-tested net layer (`src/Net/`, ~3k lines) wired into one game. It becomes an engine
module later (v1.0 of the library), once the API has survived a second game.

Anchors were read on Headstone `master` @ `69d53f9` and DingoEngine `master` @ `5ae1bae` (v0.9.0).
This file moves to the DingoNet repo (`.claude/plans/`) in phase 1 and is removed from the engine.

## Decisions (2026-10-12)

- **Name**: DingoNet, namespace `Dingo::Net`, headers as `<DingoNet/...>`.
- **Consumption**: a **prebuilt package**, laid out like the engine package Headstone vendors
  (`Headstone/scripts/fetch-engine.ps1`): release zips per configuration, unpacked to
  `vendor/DingoNet/{include, lib/<Config>, VERSION.txt}`.
- **Engine-free core**: the `DingoNet` library includes no DingoEngine header and no glm. It logs
  through a handler the game installs. A thin `DingoNet-Engine` library (a `NetLayer`, scene
  replication) comes in v0.4 and is the only part that links the engine.
- **Cross-platform from v0.1** (2026-10-13, a hard requirement): Windows (MSVC, v145) and Linux
  (GCC ≥ 13, x86-64), the engine's two platforms. Every phase builds and passes its tests on both
  (Linux locally through WSL2); there is a package and a CI job for each. Rules:
  - Platform headers (`winsock2.h`, `sys/socket.h`, `unistd.h`) only under `src/Platform/`; public
    headers are plain C++20.
  - Time from `std::chrono::steady_clock`, never `QueryPerformanceCounter` or `clock_gettime` directly.
  - Socket errors map to one `SocketError` enum (`WSAEWOULDBLOCK` and `EAGAIN`/`EWOULDBLOCK` are both
    `WouldBlock`). Linux sends use `MSG_NOSIGNAL`: a send to a closed peer otherwise raises `SIGPIPE`
    and kills the game.
  - The engine's GCC rule applies: a member named after its type spells the type qualified.
  - macOS is not a target (the engine doesn't build there), but nothing may rule it out.
- **Its own repository** (2026-10-13): `KingoBoiii/DingoNet`, cloned beside the engine at
  `D:\Git\Github\KingoBoiii\DingoNet`. Its own code, issues, `CLAUDE.md`, `.claude/plans/` and
  `.claude/reviews/`; the engine repo never vendors or builds it.
- **Its own versions**, independent of the engine's:
  - `VERSION` holds `major.minor`; CI appends the commit count as the build number and writes
    `include/DingoNet/BuildInfo.h` (`DN_VERSION_*`, `Dingo::Net::GetVersion()`), as the engine does.
  - Tags `vX.Y.Z` on release branches, a GitHub release per tag with the package zips.
  - The library version is not the wire version: the session layer's own handshake carries
    `k_SessionProtocol` (bumped only when DingoNet's session messages change), beside the game's
    protocol number and content hash. A patch release never changes the wire.
  - Engine compatibility: the core has none. From v0.4, `DingoNet-Engine` builds against a pinned
    engine package (fetched like Headstone's `fetch-engine.ps1`), and the README keeps a table of
    DingoNet version → minimum engine version.
- **Repo created** 2026-10-13 (public), phase 1 built and verified on Windows and Linux, not yet committed.

## What Headstone has today

**Liftable almost unchanged** (no game includes):
- `Net/Transport.h:29-44`: abstract `Transport` (`Send`/`Poll`/`Disconnect`/`Close`/`Describe`,
  byte counters), pumped once a frame, no threads. `PeerId` is `uint64_t`, `Delivery` is
  `Reliable | Unreliable`.
- `Net/TcpTransport.{h,cpp}`: non-blocking Winsock2, `TCP_NODELAY`, IPv4, a 4-byte LE length prefix,
  a 64 KB frame cap, a 4 MB send-queue cap, 16 connections. Read-before-send works around Windows
  dropping unread data (incl. the parting `Bye`) on reset (`TcpTransport.cpp:228-231`). Windows only.
- `Net/Conditioner.h`: wraps a transport with lag, loss and jitter for tests, on its own RNG stream.
- `Net/Session.{h,cpp}`: handshake (`Hello` → `Welcome | Reject`), seats (slot 0 = host), 1 Hz
  ping/pong with smoothed RTT, 5 s silence timeout, 5 s handshake timeout, `Bye`, `SetAccepting`,
  and `Joined/Left/Welcomed/Rejected/Lost` events. Game messages pass through tagged with a slot.
- In `Net/Protocol.{h,cpp}`: `ByteWriter`/`ByteReader` (`:70-125`; LE, bounds-checked with a sticky
  fail flag checked once via `Ok()`), the quantizers `Coord`/`Angle16`/`Angle8`/`Unit8`, `PeekType`,
  `CleanName`, and the session messages' encode/decode.
- `Net/SteamTransport.{h,cpp}` + `Platform/SteamLobby.{h,cpp}`: `ISteamNetworkingMessages` on
  channel 0, lobby-member allow-list, stale-queue drain on create (`SteamTransport.cpp:66-76`).

**Headstone-specific in `Session`** (what v0.1 parameterizes): `MAX_SLOTS = 4` (`Session.h:19`),
`PROTOCOL_VERSION` as a global (`Protocol.h:35`), literal English reason strings, `CleanName`'s
HUD-font charset, `__DATE__ __TIME__` as the build string, session message ids `>= 64`
(`Protocol.h:68`).

**Stays in the game**: the game messages in `Protocol.{h,cpp}`, `Game/Replication.*`,
`Game/HeadstoneNet.cpp` (1368 lines: dispatch `switch`, host shot validation, lobby flow, snapshot
build), `Net/Mirror.*` until v0.2 makes it generic.

**Lessons the library builds in**
1. Unreliable packets outlive state transitions (late snapshots after `RunStart` or on Game Over,
   `HeadstoneNet.cpp:583-593`; the `Life` counter; the dead-ID set). → a session **epoch** (v0.2).
2. Players read the transport's English error text. → **reason codes**, the game owns the strings (v0.1).
3. Steam drops a whole fragmented unreliable message. → `MAX_UNRELIABLE_BYTES = 1100` is a library
   constant and `Send` asserts on it (v0.1).
4. `PROTOCOL_VERSION` is bumped by hand for map layouts too (19 bumps; one merge left it stale,
   `Protocol.h:26`). → `Hello` carries a game-supplied **content hash** next to the version (v0.1).
5. Flags bytes are full and new fields append order-dependently. → not solved in v0.1; revisit with
   the message registry (v0.2).

## Versions

| Version | Scope | Done when |
|---|---|---|
| **v0.1 Extract** | Bytes, Transport, TCP (Winsock + POSIX), Loopback, Conditioner, Session (configurable), Steam as a source drop, Windows and Linux packages + CI | Headstone runs on the package with no gameplay change; its net tests and a 2-instance soak pass; DingoNet-Tests pass on both OSes, a Windows host and a Linux client included |
| v0.2 Structure | Message registry (replaces the dispatch `switch`), session epoch, generic `Mirror<T>` interpolation | `HeadstoneNet.cpp` roughly halves |
| v0.3 UDP | Reliable-UDP transport (ENet proposed) | LAN play over UDP without Steam, on Windows and Linux |
| v0.4 Engine bridge | `DingoNet-Engine`: `NetLayer`, UUID-keyed scene replication (`IDComponent`, `Scene::GetEntityByUUID`, `Scene.h:208`) | 2–4 player co-op retrofitted onto a second example (the roadmap showcase) |
| v1.0 Module | Packaged as an optional engine module, documented | Listed under the engine roadmap's Modules |

## v0.1 — Extract

One phase per run, one commit per phase. Phases 1–5 land in the new DingoNet repo, phase 6 in
Headstone.

### Phase 1 — repo skeleton
- `premake5.lua`: workspace `DingoNet`, configs `Debug`, `Release`, `Distribution` (the configs the
  engine package ships), x86_64, C++20, `staticruntime "Off"`, `toolset "v145"` on Windows (matches
  `Headstone/premake5.lua:24,88` so the game links both packages with one CRT); `premake5 gmake`
  with GCC ≥ 13 on Linux, outputs under `build/bin/<Config>-linux-x86_64/` like the engine's.
- `Generate-Windows.bat` and `Generate-Linux.sh`, as in the engine.
- Projects: `DingoNet` (StaticLib, `include/DingoNet/`, `src/`) and `DingoNet-Tests` (ConsoleApp).
- `.editorconfig` and a `CLAUDE.md` copied from DingoEngine's conventions (`CLAUDE.md:65-98`):
  `m_`/`s_` prefixes, UPPER_SNAKE constants, tabs, braces on new lines, load-bearing comments only.
- `include/DingoNet/Log.h`: `enum class LogLevel { Trace, Info, Warn, Error }`,
  `using LogHandler = void(*)(LogLevel, std::string_view)`, `SetLogHandler`. Default writes to stderr.
  The game routes it to `DE_INFO_TAG` etc.
- `VERSION` file (`0.1`) and a checked-in placeholder `include/DingoNet/BuildInfo.h` (`0.1.0`) that CI
  overwrites; `Dingo::Net::GetVersion()` reads it.

### Phase 2 — bytes
- `include/DingoNet/Bytes.h`: `ByteWriter`/`ByteReader` from `Protocol.h:70-125` and the quantizers,
  unchanged in encoding. No glm: callers pass floats.
- `PeekType` becomes `PeekU8`; message-type enums stay the game's.
- Tests: round-trip every primitive and quantizer at their edges; a truncated read sets the fail
  flag and returns zeros.

### Phase 3 — transports
- `Transport.h` as today, plus:
  - `PeerEvent::Reason` becomes `DisconnectReason` (`Closed`, `TimedOut`, `Refused`, `Reset`,
    `ConnectFailed`, `Error`) and `std::string Detail` for logs only.
  - `inline constexpr size_t MAX_UNRELIABLE_BYTES = 1100;`; `Send` logs an error and drops an
    oversize unreliable packet.
- `TcpTransport` moved, its socket calls behind `src/Platform/Socket.h` with two implementations:
  `WinSocket.cpp` (Winsock2, `WSAStartup` refcounted, `FIONBIO`) and `PosixSocket.cpp` (BSD sockets,
  `fcntl(O_NONBLOCK)`, `MSG_NOSIGNAL`). Premake picks one per system. The read-before-send order
  (`TcpTransport.cpp:228-231`) stays on both. Caps become `TcpParams` fields with today's values as
  defaults.
- `LoopbackTransport` (new): `CreateLoopbackPair(host, client)`, in-process queues, for tests and for
  playing through the whole pipeline solo (Headstone's `--net-loopback`).
- `Conditioner` moved as-is, wrapping any `Transport`.
- Tests, on Windows and on Linux: TCP host/client on `127.0.0.1` exchanging framed packets; a peer
  closing mid-send (no `SIGPIPE`, a `Disconnected` event); Loopback + Conditioner at 200 ms / 5 % loss
  delivering every reliable packet in order. Once by hand: a Windows host and a WSL2 client.

### Phase 4 — session
- `SessionParams`:
  - `int MaxSlots = 4` (1–32; seats become a `std::vector`)
  - `uint16_t Protocol`, `uint32_t ContentHash` (game-computed, e.g. over map layouts)
  - `std::string Build` (informational, logged on mismatch)
  - `double Timeout = 5.0`, `PingInterval = 1.0`, `HandshakeTimeout = 5.0`
  - `std::function<std::string(std::string_view)> NameFilter` (default: printable ASCII, 16 chars)
- Reasons become codes: `RejectReason { ProtocolMismatch, ContentMismatch, NotAccepting, Full }`,
  `LeaveReason { Bye, TimedOut, Kicked, TransportLost }`. `Event` carries the code plus the
  transport's `Detail`.
- Session message ids move to `0xF0–0xFF`; games own `0x00–0xEF`. `Hello` gains `ContentHash`.
  This is a wire change: Headstone bumps to protocol 20 in phase 6.
- The session messages' encode/decode move here from Headstone's `Protocol.cpp`.
- Tests: handshake, full (MaxSlots + 1 clients), protocol and content mismatch, `SetAccepting(false)`,
  timeout via a frozen Conditioner, `Bye`, churn (join/leave 100×) — all over Loopback.

### Phase 5 — package and CI
- `scripts/package.ps1`: builds each config, merges nothing (no vendor libs), zips
  `DingoNet-<ver>.<build>-<Config>-windows-x86_64.zip` with `DingoNet.lib`, `.pdb`, `include/`,
  `steam/`, `VERSION.txt`; `scripts/package.sh` does the same on Linux into
  `DingoNet-<ver>.<build>-<Config>-linux-x86_64.zip` with `libDingoNet.a`.
- **Steam ships as source** in `steam/SteamTransport.{h,cpp}`: the game compiles it against its own
  Steamworks SDK. The package and its CI need no Steam SDK, and a non-Steam game ignores the folder.
  `SteamManager` is replaced by an `ISteamNetworkingMessages*` passed in; lobby code stays in the game.
- `.github/workflows/release.yml` on a `v*` tag, mirroring the engine's `build-release.yml`, with a
  Windows and an Ubuntu job.
- `DingoNet-Tests` runs in CI on every push, on `windows-latest` and `ubuntu-latest`.

### Phase 6 — Headstone on DingoNet (Headstone repo)
- `scripts/fetch-dingonet.ps1`, modelled on `fetch-engine.ps1`, with a `-Source <path>` switch that
  builds a local DingoNet checkout instead (the inner loop while both change).
- `premake5.lua`: include `vendor/DingoNet/include`, link `DingoNet`, add
  `vendor/DingoNet/steam/*.cpp` under `HEADSTONE_STEAM`.
- Delete `Net/Transport.h`, `TcpTransport.*`, `Conditioner.h`, `Session.*`, `SteamTransport.*`; strip
  `ByteWriter`/`ByteReader`/`CleanName`/session messages from `Protocol.*`.
- Install the log handler, pass `SessionParams { MaxSlots = 4, Protocol = 20, ContentHash = <map hash>,
  NameFilter = CleanName }`, map `RejectReason`/`LeaveReason` to the existing translated strings.
- `PROTOCOL_VERSION = 20` (session ids moved, content hash added). From here map layouts change the
  content hash, not the protocol version.
- **Verify** (Headstone's own tools): `--net-selftest`; `--net-loopback=100,5`; two instances via
  `capture.ps1` with join, leave, `kill` and `freeze`; `--net-churn`; the 20-minute soak at 200 ms /
  5 % loss from `coop-plan.md`; one Steam lobby session between two accounts (manual).
- Headstone itself stays Windows-only for now (it links `Bcrypt` and builds only with MSVC); DingoNet's
  Linux side is proven by its own tests in phases 3–5, not by Headstone.

**Estimate**: phases 1–5 about a day and a half of agent time, Linux included; phase 6 about half a
day plus the manual two-machine Steam check.

**Who does it** (per the global delegation rules): Opus for phases 3, 4 and 6 (transport, session,
wire change — high-risk netcode); Sonnet for phases 1, 2 and 5; an Opus review of the v0.1 diff
before the first tag.

## Open questions
1. **ENet vs. GameNetworkingSockets for v0.3.** ENet: one small C library, trivial in premake, no
   encryption. GNS: same API family as the Steam transport (one code path online and offline), but
   pulls in protobuf and OpenSSL/libsodium. Proposed: ENet.
2. **macOS**: the engine doesn't build there yet, but the POSIX socket code would cover it.
   Proposed: keep the code portable, with no macOS package or CI until the engine has one.
3. **Engine roadmap**: update `ROADMAP.md:419-426` to name DingoNet and link its repo once it exists.
