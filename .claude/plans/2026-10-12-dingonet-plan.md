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
- `scripts/windows/{Generate,Build,Test}.bat` and `scripts/linux/{Generate,Build,Test}.sh` (2026-10-13: every script lives under `scripts/<platform>/`).
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

**As built (2026-10-13)**: `Bytes.h`/`Bytes.cpp`, 10 tests in `tests/BytesTests.cpp`, Opus-reviewed
(no code findings; its test and lifetime suggestions applied). Byte-identical to Headstone for every
input where Headstone's output was defined, pinned by a golden-bytes test of the rounding and wrap
boundaries. Deliberate differences:
- NaN encodes as 0 in `Coord`/`Unit8`, NaN and ±inf as 0 in the angles; ±inf `Coord`/`Unit8` clamp.
- `Coord` clamps before rounding: the same bytes for every value Headstone could round, and the
  clamped value (not `lround` overflow, unspecified, `LONG_MIN` on MSVC past ±21,474,836 m) beyond.
- `U32`/`U64` check every byte first, so a truncated read returns 0 (Headstone returned the low half).
- The reader holds a `std::span`; constructing one from a temporary vector (const or not) is deleted.
  A span or pointer to a temporary still dangles, as in Headstone.
- `PeekU8(std::span)` returns 0 for an empty packet; `ByteWriter::Clear()` added.

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

**As built (2026-10-13)**: `Transport.h`, `TcpTransport.h`, `LoopbackTransport.h`, `Conditioner.h`; sockets behind
`src/Platform/Socket.h` (`WinSocket.cpp` / `PosixSocket.cpp`, one per system, `Socket.cpp` shared); 22 tests in
`tests/TcpTests.cpp` and `tests/TransportTests.cpp`, passing on Windows and Linux, Debug and Release (about 2.4 s on
Windows, where a refused connect takes Windows' SYN retries; under 0.1 s on Linux). An Opus review's 12 findings
are fixed. The frame format, the caps and the read-before-send order are Headstone's; built without `MSG_NOSIGNAL`,
the Linux run dies of `SIGPIPE` (exit 141) in `TcpSocketSendsAfterAResetFailWithoutSigpipe`, the SIGPIPE coverage
(the transport reads first, so its own reads meet a reset before a send can).
Cross-OS: a WSL2 (NAT) host and a Windows client exchanged 200 echoed packets (`--tcp-host`/`--tcp-client` on
`DingoNet-Tests`); a Windows host was not tried, since the host firewall has no inbound rule for the WSL interface
and would block it (and prompt). Deliberate differences:
- `Send` takes `std::span<const uint8_t>` (a `std::vector` converts). `PeerEvent` carries a `DisconnectReason` and
  a `Detail` for logs instead of English text: `Refused` (RST on connect), `ConnectFailed` (anything else before
  connecting, the 5 s timeout included), `Closed` (FIN), `Reset` (reset or abort), `TimedOut` (`ETIMEDOUT` on a live
  socket), `Error` (bad frame, full send queue, other socket errors).
- `TcpTransport` has no `SetConditions` and no built-in conditioner: `Conditioner` is a `Transport` that owns the
  transport it wraps and conditions what is sent through it, so it works over Loopback (and Steam) too. It adds
  jitter (a reliable packet's release time never precedes the previous reliable one's), a `Seed`, a `Clock` hook
  (Headstone's `--net-loopback` drives its queue with the game's net clock) and a pass-through while inactive.
  Its `Disconnect`/`Close` release what it holds first, as Headstone's `HangUp` popped the link.
- The caps are `TcpParams` (same defaults); `DEFAULT_PORT` stays in the game; port 0 binds any free port and
  `GetPort()` reports it.
- A reliable packet over the frame cap is logged and dropped by the sender (Headstone sent it and the receiver
  dropped the connection as a bad frame); an unreliable one over `MAX_UNRELIABLE_BYTES` is logged and dropped by
  every transport. A bad incoming frame, a full send queue, a connection past `MaxConnections` and a failed
  `accept` now log a warning (Headstone was silent).
- A connect that fails at once returns a transport that reports `Refused`/`ConnectFailed` at its first `Poll`
  (Headstone returned null with an error); a bad address or a socket that can't be made still returns null.
- Linux listeners set `SO_REUSEADDR`; Windows keeps `SO_EXCLUSIVEADDRUSE`. Sent bytes leave the queue once per
  `Poll` instead of after every `send` call (the same bytes, no quadratic erase).
- `LoopbackTransport` is new: `Listen()`, `Connect(host)` (many clients), `CreateLoopbackPair()`; it has no frame
  cap, so a reliable packet over 64 KB passes there and is dropped over TCP.
- `Disconnect` flushes the whole queue over later `Poll`s and shuts our side only once it is empty (Headstone sent
  once without waiting and cleared the rest, cutting a big parting frame); `Close` flushes for at most 0.25 s.
- Reads are capped at `MaxReadBytesPerPoll` (256 KB) per connection per `Poll` (Headstone read until `WouldBlock`, so
  a flooding peer could stall a frame); the queue cap is checked after the flush, not before; `MaxConnections`
  counts live connections only, so a kicked player can reconnect while the old socket lingers.
- Sockets are close-on-exec / not inheritable; durations are clamped (NaN 0, at most 1e6 s) before the chrono cast.
- `Conditioner` drops what it holds for a peer the inner transport reports lost, and orders a reliable packet after
  the previous one only while that one is still held (a clock that jumps back doesn't stall it).

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

**As built (2026-10-13)**: `Session.h`, `src/Session.cpp`, the wire in `src/SessionMessages.{h,cpp}`; 27 tests in
`tests/SessionTests.cpp` (62 in all), passing on Windows and Linux, Debug and Release, adding about 0.2 s (time comes
from `SessionParams::Clock`; one handshake, two game messages and a `Bye` go over TCP on 127.0.0.1). Wire, all
little-endian: `Hello` F0 `u16 k_SessionProtocol, u16 Protocol, u32 ContentHash, str name, str build`; `Welcome` F1
`u8 slot, u8 maxSlots, str hostName, u8 n, n × (u8 slot, str name)`; `Reject` F2 `u8 RejectReason, u16 session, u16
protocol, u32 content, str build` (the host's); `Ping`/`Pong` F3/F4 `u32 ms`; `Bye` F5 `u8 LeaveReason` (`Bye` or
`Kicked`); `SeatJoined` F6 `u8 slot, str name`; `SeatLeft` F7 `u8 slot, u8 LeaveReason`. Golden bytes pin `Hello`,
`Welcome` and `Reject`. Deliberate differences from Headstone:
- `Update()` takes no time: `steady_clock`, or the `Clock` hook (Headstone passed `WallSeconds()`). `HandshakeTimeout`
  is its own setting; a client's runs from `StartClient` and restarts at the transport's `Connected` (Headstone's
  started only at `Connected`, so a transport that never reported it relied on its own connect timeout).
- Clients learn the roster: `Welcome` lists the other seated clients and the host sends `SeatJoined`/`SeatLeft`, so a
  client gets `Joined`/`Left` for the other clients and `SlotTaken`/`NameOf`/`SeatedCount` are right on every machine
  (Headstone's clients knew only seat 0 and their own, and learnt the rest from its `LobbyState`). Phase 6: Headstone's
  `Joined`/`Left` handler takes them on clients too (its `HostBroadcastLobby` is host-only already; `m_Players.Join`
  on a client is new).
- `Kick(slot)` is new (sends `Bye(Kicked)`; `Left(Kicked)` on the host and the other clients, `Lost(Kicked)` on the
  kicked one). `Bye` carries a reason byte; Headstone's was the bare id.
- `Reject` carries a code and the host's versions and build instead of English text; `Detail` holds both sides'
  versions for the log. A `Hello` whose session protocol differs is turned away (`ProtocolMismatch`) without
  decoding the rest. `Lost` with `Slot` -1 is Headstone's "could not reach the host".
- `NameFilter` runs on the session's own name and on every name a client receives (the host's, the roster), not only
  on a `Hello`; Headstone sent its own name raw. `Welcome` carries the host's `MaxSlots`: a client adopts a smaller
  count and turns a larger one away (`Rejected(ProtocolMismatch)`, then a `Bye`), so no slot past its own arrays.
  Roster joins carry `Event::Existing`; `Event::Transport` holds the transport's `DisconnectReason` on `TransportLost`.
- Decoding is strict (trailing bytes fail too) and every malformed, wrong-side or unknown session packet is logged as
  a warning and dropped (Headstone dropped silently, and an unknown id ≥ 64 reached the game as a game message).
  A slot outside the seats, a duplicate roster slot or a `Welcome` for slot 0 is malformed. A pong timestamp from the
  future is ignored; the smoothed RTT starts from its first sample even when that is 0 ms.
- A game `Send`/`Broadcast` whose first byte is in `0xF0–0xFF` logs an error and is dropped.
- `SetParams` is refused while active (`SetProtocol` still works at any time, for `--net-proto`); the destructor
  calls `Stop()`; `PeerCount()`, `MaxSlots()`, `IsAccepting()` and `GetParams()` are new.
- After an Opus review (11 findings, all fixed): the transport's events run in order per id (a disconnect followed
  by a connect of the same id, a SteamID back, drops the old peer before the new one's packets; a plain disconnect
  still waits for the packets), and a packet from an untracked peer makes the host `Disconnect` it; a slot's queued
  game messages are dropped with its `Left`, so none reaches the next occupant; bad session packets warn once per
  peer, problem and id, and a host hangs up (`Bye(Kicked)`) after 32; a peer's build is cut to 64 printable ASCII
  before it reaches `Detail`; `PingInterval` is clamped to `Timeout / 2`; `Describe` lists only measured round
  trips; `SessionParams::UnseatedPacket` lets a host answer a game-range packet from an unseated connection (an
  older wire's Hello) with a raw reply before hanging up, where otherwise that client only times out. A `Bye` from an
  unseated peer is ignored (the handshake timeout or the transport's disconnect clears it).
- The timeout tests freeze one side with the clock hook instead of a frozen `Conditioner`: a `Conditioner` releases
  what it holds when its owner hangs up, so the side that times out second always reads that as `TransportLost`.

### Phase 5 — package and CI
- `scripts/windows/Package.bat`: builds each config, merges nothing (no vendor libs), zips
  `DingoNet-<ver>.<build>-<Config>-windows-x86_64.zip` with `DingoNet.lib`, `.pdb`, `include/`,
  `steam/`, `VERSION.txt`; `scripts/linux/Package.sh` does the same on Linux into
  `DingoNet-<ver>.<build>-<Config>-linux-x86_64.zip` with `libDingoNet.a`.
- **Steam ships as source** in `steam/SteamTransport.{h,cpp}`: the game compiles it against its own
  Steamworks SDK. The package and its CI need no Steam SDK, and a non-Steam game ignores the folder.
  `SteamManager` is replaced by an `ISteamNetworkingMessages*` passed in; lobby code stays in the game.
- `.github/workflows/release.yml` on a `v*` tag, mirroring the engine's `build-release.yml`, with a
  Windows and an Ubuntu job.
- `DingoNet-Tests` runs in CI on every push, on `windows-latest` and `ubuntu-latest`.

**As built (2026-10-13)**: pushed as `257f93e`, Opus-reviewed (1 High, 2 Medium, 8 Low, all fixed);
the first CI run passed all six jobs. Differences from the spec above:
- Zips are `DingoNet-<MAJOR.MINOR.PATCH>.<build>-<Config>-<os>-x86_64.zip` (the engine's `<tag>.<commits>`
  naming, so `fetch-dingonet.ps1` can mirror `fetch-engine.ps1`); no `.pdb` (`/Z7` embeds symbols).
- `release.yml` runs when a release is published (changed from a `v*` tag push on 2026-10-14), and by hand (`workflow_dispatch`, `tag` input), creating the release when it is
  missing, since the owner creates engine releases by hand after tagging.
- CI's Windows jobs build with v143 (`WINDOWS_TOOLSET`), like the engine's; built and warning-free
  locally with VS 2022 in all three configs.
- `SteamTransport::Create` takes a `log` callback and an `isAlive` hook (Headstone checked
  `SteamManager::IsAvailable()`); close reasons map to `ConnectFailed` rather than `Refused`; send
  results are checked; a peer we disconnected is ignored for 1 s instead of reappearing (3 s at first; the phase 6 review showed it swallowed a quick rejoin).

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

**As built (2026-10-13)**: Headstone `feat/dingonet` @ `4da5842` (worktree
`Headstone/.claude/worktrees/dingonet`), committed, not pushed (Headstone pushes only when asked).
Opus-reviewed (1 High, 1 Medium, 6 Low; all fixed or noted). No DingoNet API change was needed; the
review led to two DingoNet fixes (`23c2c2c` scripts no longer pause when called with arguments,
`1c99543` the Steam ignore window is 1 s).
- `ContentHash` = `World::LayoutHash`, over the map builders' exact inputs (prop fields, mesh bounds,
  AddBox/AddCircle/AddRiseSpot arguments, map constants, `Tuning::ENEMY_RADIUS`), computed once behind
  the loading screen (58–92 ms). Map code uses `Layout::Sin/Cos/Atan2/...` (`MapKit.h`, `+ - * /` and
  `sqrt` in double) because the x64 CRT's trig takes an FMA path on some CPUs, which could give two
  players on one build different hashes.
- `UnseatedPacket` answers a protocol-19 Hello (id 0x40) with the old Reject bytes, so an old client
  sees "VERSION MISMATCH". The reverse (a new client, an old host, over TCP) still times out; Steam
  lobbies catch it first through their `proto` data.
- Clients now get join/leave toasts for other clients (`Existing` roster entries toast nothing); the
  mirror re-seats only slots the session lists, which ended a 2 s "PLAYER N" flicker after a leave.
- Verified hidden: `--net-selftest`, `--loc-selftest`, `--net-loopback=100,5`, 2- and 3-player TCP
  co-op with a kill, `--net-churn`, a `--net-proto=19` client. Open: the 20-minute soak, the
  two-account Steam session, and Headstone's CI, which 404s until a DingoNet `v0.1.0` release exists
  (`fetch-dingonet.ps1` pins the version and build in two lines).

**Released (2026-10-14 to 10-21)**: `v0.1.0` published 2026-10-14 from `b815b9e` (build 11; the release
workflow built and uploaded all six zips). Headstone merged phase 6 as PR #97 on 2026-10-21, but behind a
premake option, `--dingonet` (`HEADSTONE_DINGONET`): the default build keeps Headstone's own net layer in
`src/Net/Legacy/` until the DingoNet build passes a two-account Steam session and Windows↔Linux co-op
(Headstone's `dingonet-flag-plan.md`, Phases 4–5). Both builds share protocol 20, the content hash and
`Layout::` trig; Headstone's Linux build fetches the Linux zip (`fetch-dingonet.sh`). The owner (Windows) played the
`--dingonet` build with a friend on the Linux build on 2026-10-21, through a Steam lobby on their own accounts,
and it worked as expected: the first run of the Steam transport, on both OSes, and of Windows↔Linux co-op.
That passes both of Headstone's Phase 4 gates.

**v0.1.1 (2026-10-22)**: the Opus review of the whole v0.1 diff (`.claude/reviews/2026-10-22-v0.1-review.md`:
1 High, 2 Medium, 7 Low) fixed on `master`, one commit per finding, not yet tagged. Wire-compatible with
v0.1.0 (`k_SessionProtocol` stays 1, golden bytes unchanged); the one API addition is
`Session::Event::LastMessages`. Session: a host sends `Bye(TimedOut)` before every timeout drop and Pongs
only seated peers (a Steam client it timed out no longer stays "connected" forever); a `Hello` from a seated
peer frees the seat and joins fresh (a Steam rejoin under the same SteamID); a leaver's queued messages move
onto its `Left`; a host keeps at most 16 connections waiting for a `Hello`. TCP's `MaxConnections` defaults
to 64. `SteamTransport` makes no Steam call while `isAlive` is false and is inert after `Close`, tested by
`DingoNet-SteamTests` (`--steam-sdk`, a fake `ISteamNetworkingMessages`). The Linux library is `-fPIC`; the
`.bat` files pause only under Explorer. Tests: 62 → 71 on both OSes, plus 5 Steam tests.

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
