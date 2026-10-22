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
| v0.2 Structure | Message registry (replaces the dispatch `switch`), session epoch, generic `Mirror<T>` interpolation | Headstone dispatches through the registry, has no hand-written stale-packet guards, and builds its mirror on the toolkit |
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

A second review of those fixes (same file, R1–R5, 5 Low) is fixed too: a timeout `Bye` only to seated peers,
joiners kept through a connection flood with lingering sockets capped, a session `Hello` ending Steam's 1 s
ignore window, `Session::Event::FromBye` (the second API addition) and two TCP tests that skip rather than
flake. Tests: 75 on both OSes, plus 6 Steam tests.

**Estimate**: phases 1–5 about a day and a half of agent time, Linux included; phase 6 about half a
day plus the manual two-machine Steam check.

**Who does it** (per the global delegation rules): Opus for phases 3, 4 and 6 (transport, session,
wire change — high-risk netcode); Sonnet for phases 1, 2 and 5; an Opus review of the v0.1 diff
before the first tag.

## v0.2 — Structure (planned 2026-10-22)

Three engine-free pieces, each from a pattern Headstone hand-rolls today. All are opt-in, so a game on
v0.1's API keeps compiling; only the epoch changes the wire.

**What Headstone does today** (`main` @ `b81e3f6`)
- **Dispatch**: `HeadstoneLayer::OnNetMessage` (`HeadstoneNet.cpp:424-760`) is one `switch` over 15
  `MsgType`s. Every case repeats the same three things: decode, check the direction (`if (!host ...)`),
  check the screen. A second `switch` (`:1354`) does the loopback path. Encoding is per-message free
  functions in `Protocol.cpp` (1098 lines).
- **Stale packets**: unreliable messages outlive run changes. Headstone guards by hand in four places:
  - Snapshots dropped on menus and the Game Over card (`:576-588`).
  - The host sends none from Game Over (`:791`).
  - Clients send no `PlayerState` from Game Over (`:1211`).
  - A per-player `Life` counter catches respawns (`:435`).
  Each guard was added after a bug (Instant Restart D3, 2026-10-01).
- **Mirror**: `Net/Mirror.{h,cpp}` (352 lines) is already half generic. It has per-id tracks of host-time
  samples, a render clock `DELAY` (0.1 s) behind, at most `MAX_EXTRAPOLATE` (0.15 s) of extrapolation,
  a `STALE` (2 s) forget and a `HISTORY` (1 s) trim. On top sit the game-specific parts: deaths that
  fall due at render time, a stepped header/pickups timeline, and `Settle()` for Game Over.

### Phase 1 — message registry (~3 h, Sonnet)

`include/DingoNet/Messages.h`, header-only templates over `Bytes.h`:
- A message type declares its id, direction and delivery, and how to write and read itself:
  ```cpp
  struct PlayerState
  {
      static constexpr uint8_t Id = 3;
      static constexpr Direction Dir = Direction::ToHost;     // ToHost, ToClients, Both
      static constexpr Delivery Send = Delivery::Unreliable;
      void Write(ByteWriter& w) const;
      bool Read(ByteReader& r);
      ...fields
  };
  ```
- `Encode(msg)` writes the id and the body. An unreliable message over `MAX_UNRELIABLE_BYTES` fails
  there, at the sender, with the message's name in the log.
- `MessageRouter`:
  - `router.On<PlayerState>([](int slot, const PlayerState& m) { ... })` registers a handler.
  - `router.Dispatch(slot, data, isHost)` peeks the id, drops a message arriving at the wrong side,
    decodes, and calls the handler.
  - It counts per id: received, sent, bytes, dropped for direction, dropped as malformed. That is the
    data for a net-debug HUD.
  - An id registered twice, or one in `0xF0-0xFF`, is an error at registration.
- `Session::Send(slot, msg)` / `Broadcast(msg)` overloads take a message and use its declared delivery.
- The wire is whatever `Write` writes, so Headstone's existing encodings move over byte for byte.
  Its golden-bytes tests keep passing.
- Tests:
  - round trip;
  - wrong-direction drop;
  - truncated and trailing-byte drops;
  - the oversize unreliable message;
  - duplicate and reserved ids;
  - counters.

**As built (2026-10-22)**: `Messages.h`, `src/Messages.cpp`; 93 tests in all, 18 of them new in `tests/MessageTests.cpp`. No wire change (`k_SessionProtocol` stays 1).
- A `NetMessage` concept (default-constructible; `Id` an integer or enum 0-255, static_asserted; `Dir`, `Send`, `Write`, `Read`; optional `Name`, else the hex id in logs). `Encode(msg)` returns `std::optional<std::vector<uint8_t>>`: `nullopt` plus an error naming the message for an oversize unreliable one or a reserved id.
- `MessageRouter`: `On<T>(handler)` (false plus an error for a duplicate or reserved id), `Dispatch(slot, data, isHost)` (true if a handler ran; unknown id, empty data, wrong side, short read or trailing bytes are dropped), `NoteSent<T>(bytes)`, `Stats(id)`, `Registered`, `KnownIds` (handler or sent), `NameOf`, `DroppedUnknown`, `ResetStats`. The 256-entry table is on the heap, allocated once, so a handler may call `On` and `Dispatch`; the router is not copyable or movable.
- `Session::SetRouter(MessageRouter*)` (not owned, null = none), typed `Send(slot, msg)` / `Broadcast(msg, exceptSlot)` in the message's delivery. A host sending a `ToHost` message or a client a `ToClients` one logs an error and drops it. The span `Send` now returns `bool` and `Broadcast` an `int` (seats reached); callers ignoring them compile as before.
- Counters: `Received`/`BytesIn` only when a handler ran; bytes include the id byte; `Sent`/`BytesOut` only for a packet handed to the transport, one send per `Broadcast` that reached a seat; direction and malformed drops per id; there is no router `Encode`.
- Beyond the plan: `SetRouter` (counters need the router in the send path); the reserved-id refusal is a runtime error, not a `static_assert`, so it stays testable; `FIRST_SESSION_MESSAGE`/`IsSessionMessage` moved from `Session.h` to `Messages.h`; handlers must be copyable (stored in a `std::function`).

### Phase 2 — session epoch (~3 h, Opus; wire change)

- The host calls `Session::BeginEpoch()` at every state change where old packets must die: run start,
  Game Over, back to the lobby.
- The epoch is a `u8` the host bumps and sends reliably as a new session message `Epoch` (`0xF8`).
  Clients raise `Event::EpochChanged`.
- With `SessionParams::Epochs = true`, every game message travels as `Stamped` (`0xF9`): `[0xF9][epoch][game bytes]`.
  - The receiver drops any stamp that isn't its current epoch, both older and not yet announced.
  - The game sees the same bytes as today; the session strips the stamp.
  - Reliable messages are stamped too: a client's reliable sent before it saw the new epoch is just
    as stale.
  - Queued messages from the old epoch are dropped at `BeginEpoch` (host) or `Epoch` (client).
- **Cost:**
  - 2 bytes per game message.
  - The game's unreliable limit becomes `Session::MaxUnreliablePayload()` = 1098. Headstone's
    snapshot parts reach about 1092 bytes, so its room formula shrinks by 2.
- `k_SessionProtocol` goes 1 → 2. Hello's leading session protocol already lets a v0.1 and a v0.2 peer
  turn each other away with `ProtocolMismatch`, as v0.1 designed for.
- The same bump also fixes v0.1 review F4: `SeatLeft` carries the leaver's `DisconnectReason`, so
  `Event::Transport` is right on clients too.
- Epochs replace Headstone's four run-change guards. They do not replace `Life`: a respawn is
  per-player and not a session-wide change.
- Tests over a `Conditioner` with jitter, so unreliable messages overtake reliable ones:
  - a straggler from the old epoch is dropped on both sides;
  - a new-epoch packet that beats its `Epoch` message is dropped;
  - queued messages are cleared;
  - the `u8` wraps;
  - with `Epochs = false` the game-message bytes on the wire are exactly v0.1's (`Welcome` and `SeatLeft` changed under protocol 2, and `Send` now refuses an oversize unreliable packet and returns whether it was sent);
  - a v0.1 peer gets `ProtocolMismatch`.

**As built (2026-10-22)**: `Session.h`, `Session.cpp`, `SessionMessages.{h,cpp}`; 17 new tests (`tests/EpochTests.cpp` 13, `SessionTests.cpp` 4), 110 in all, Debug and Release on Linux.
- Wire, `k_SessionProtocol` 2 (Hello and Reject layouts unchanged):
  - `Welcome` = `F1 slot maxSlots epochsOn(0|1) epoch hostName n (slot name)*`
  - `SeatLeft` = `F7 slot LeaveReason DisconnectReason`; the transport byte must be `Closed` unless the leave is `TransportLost`, else malformed (the host sends it that way)
  - `Epoch` = `F8 epoch`; `Stamped` = `F9 epoch game...` (game bytes non-empty)
- The host's `Epochs` rules: a client adopts the flag and epoch from the `Welcome`; its own param is only a hint for `MaxUnreliablePayload()` until then. A client never stamps or accepts a stamp before it is welcomed.
- `BeginEpoch()`: host with `Epochs` only, else a warning and nothing; bumps the `u8` (wraps), sends `Epoch` reliably to every seat, drops the `TakeMessages` queue and the `LastMessages` of `Left` events not yet taken. A client's `Epoch` does the same and raises `EpochChanged` (`Event::Epoch`). `GetEpoch()`, `EpochsOn()`, `StaleDropped()` (debug counter).
- Stale stamps (older or not yet announced) are dropped silently, counted in `StaleDropped()`, never toward the 32-bad hang-up. An unstamped game packet from a seated peer with epochs on, a `Stamped` shorter than 3 bytes or carrying a session id, and a `Stamped`/`Epoch` where epochs are off are malformed/unexpected (warned once per peer and id as `0xF9`/`0xF8`). A `Stamped` from an unseated host peer is unexpected (older wires never stamp); its raw packets still reach `UnseatedPacket`.
- `MaxUnreliablePayload()` = 1098 with epochs on, else 1100. The session now checks the unreliable limit itself in both modes: an oversize span `Send` returns false (the transport used to drop it silently); typed sends name the message and count nothing. An empty packet is refused (error) with epochs on; with epochs off the wire is v0.1's, byte for byte.
- Compatibility tests: a v0.1 Hello (protocol 1) at a v0.2 host gets `Reject(ProtocolMismatch)` and a hang-up (no `Bye`, as in v0.1); a v0.2 client given a hand-written v0.1 `Reject` raises `Rejected(ProtocolMismatch)` at once. A v0.1 host compares Hello's leading session protocol first and rejects a protocol-2 Hello the same way.
- Source compatibility, the one note: `Event::Kind::EpochChanged` is new, so a v0.1 game's exhaustive `switch` over `Event::Kind` fails under `-Wswitch` with `-Werror` (GCC, clang; MSVC's C4061/C4062 are off by default) until it adds a case or a `default`.
- Contract (v0.2 review F1, F2, F3): `BeginEpoch()` first, then the new state's messages (a message sent before it may be dropped); events before messages each frame (`TakeEvents`, then `TakeMessages`); `MaxUnreliablePayload()` follows `Params.Epochs` before the session starts.
- Beyond the plan: the stale counter, `EpochsOn()`, the 0xF9/0xF8 warning ids, the strict `SeatLeft` combination, the empty-packet refusal. Whether a same-SteamID rejoiner's old traffic dies is the game's call (`BeginEpoch`).

### Phase 3 — the mirror toolkit (~3 h, Opus)

`include/DingoNet/Mirror.h`, header-only, no glm. The game supplies `Interpolate(const T& a, const T& b,
double t)` (found by ADL) and, for extrapolation, `Extrapolate(const T& a, const T& b, double t)`, `t` > 1.

| Piece | What it does | Headstone today |
|---|---|---|
| `RenderClock` | Host-time estimate from snapshot stamps (`HostNow`), render time `Delay` behind it, `Settle()` (hold at the host's last word) | `m_LatestHostTime`, `m_RenderTime`, `Settle` |
| `Tracks<Key, T>` | Per-key sample history; `Sample(key, renderTime)` interpolates, extrapolates up to `MaxExtrapolate`, then holds; forgets keys unseen for `Stale`, trims samples older than `History` | `Track<State>`, `Push`, `Sample`, `Trim` |
| `Timeline<T>` | Stepped values (switch when the render clock reaches them) and timed events (`TakeDue()`) | header/pickups deque, `DueDeath` |

- `Tracks` handles split snapshots: each key keeps its own history, so a lost part only costs that
  part's keys a little extrapolation (Headstone's design).
- Defaults are Headstone's numbers: 0.1 / 0.15 / 2.0 / 1.0 s.
- Tests: interpolation between samples, the extrapolation cap, stale forget, trim, out-of-order
  samples, `Settle`, due events, and a jittered 20 Hz feed whose rendered path never steps more than
  a set bound. That last one is the same check as Headstone's `ProbeMirrorSmoothness`.

**As built** (2026-10-22): `include/DingoNet/Mirror.h`, `tests/MirrorTests.cpp` (30 tests after the review fixes; suite 144).
- `Timeline<T>` holds the stepped values only; timed events are `EventQueue<E>` (`Schedule`, `TakeDue`).
- `RenderClockParams`: `Delay` 0.1, `MaxSlew` 0.1, `CatchUp` 0.25, `SnapError` 0.5, `MaxDt` 0.25. `TracksParams`: `MaxExtrapolate` 0.15, `Stale` 2.0, `History` 1.0, `MinExtrapolateGap` 0.001.
- `RenderClock`: `OnSnapshot`, `Advance(dt)` (dt clamped), `HostNow`, `RenderTime`, `LatestStamp`, `HasTime`, `Settle`, `Reset`, `Restarts`. `HostNow` is an estimate that jumps up to a newer stamp and eases down at most `MaxSlew`; render time slews at `1 ± MaxSlew`, snaps forward past `SnapError`, and holds (rate 0) when ahead of its target by more than the slew band, `MaxSlew * CatchUp` (0.025 s). It goes back only on a rewind (ahead of its target by more than `RewindError`) and on a restart (stamp more than `RestartError` behind the latest); `HostNow` still resyncs down to a newer stamp more than `SnapError` behind it, so while held (or after a resync) render time can be ahead of `HostNow`. `Settle` holds `HostNow` at the latest stamp, and render moves forward to it or holds, never back, until a newer stamp. No gap-length threshold: the clock runs through gaps.
- `Tracks<Key, T, Hash>` and `Timeline` trim explicitly (`Trim(renderTime)`); `Keys()` plus `ForEach`; `Extrapolates` constant; `Interpolate`/`Extrapolate` by ADL with `double` overloads; no `Extrapolate` or no earlier sample `MinExtrapolateGap` back means hold.
- Review fixes (v0.2 review F4, F5): `RenderClockParams` gains `RewindError` 2.0 and `RestartError` 2.0. Render time goes back only on a restart (stamp more than `RestartError` behind the latest) or when ahead of its target by more than `RewindError`; a smaller overshoot holds it (rate 0) until the target catches up, and `OnSnapshot` no longer moves it. `Settle()` never moves it back.
- Eras are the game's job: clear Tracks, Timeline and EventQueue and `Reset()` the clock at an epoch change or run start.

### Phase 4 — release v0.2.0 (~1 h)

- An Opus review of the whole v0.2 diff before the tag, with its findings fixed.
- Docs: `CLAUDE.md` (the three APIs, the session messages `0xF8`/`0xF9`, protocol 2), the README, and
  an As-built note here.
- The owner publishes the `v0.2.0` release; `release.yml` builds the six zips.

### Phase 5 — Headstone on v0.2 (Headstone repo, ~half a day, Opus; after its legacy layer is gone)

- **Timing:** Headstone's default build still runs its own net layer. Its `dingonet-flag-plan.md`
  Phase 4 flips the default after 11-01, and Phase 5 deletes the old layer after one clean Playtest.
  Epochs need DingoNet's session, and the registry and the mirror live in the DingoNet package, which
  the default build doesn't link. Using them while both layers exist would mean writing everything
  twice. So this phase waits for Headstone's Phase 5.
- **Work:**
  - Each `MsgType` becomes a message type.
  - `OnNetMessage`'s `switch` becomes `router.On<…>` handlers; the loopback path uses the same router.
  - `BeginEpoch()` at `RunStart`, `RunOver` and back to the lobby; the four hand guards go.
  - `Net::Mirror` is rebuilt on `RenderClock` + `Tracks` + `Timeline`, keeping only its
    death/pickup/header glue.
  - Snapshot room: −2 bytes.
  - `PROTOCOL_VERSION` 20 → 21.
- **Done when:**
  - `--net-selftest` passes, and its golden bytes are unchanged for every message.
  - The loopback autoplay keeps the mirror's max-step at 0.010 m.
  - A 2-player TCP capture with an instant RISE AGAIN shows no last-run enemies.
  - A Steam session works.
  - Lines: the dispatch `switch` and the stale guards are gone, and `Mirror.cpp` shrinks to the game's
    glue. Expect `HeadstoneNet.cpp` to lose about a quarter, not half: most of each case's body is
    game logic that stays.

**Estimate**: phases 1–4 about a day and a half of agent time; phase 5 about half a day, after
Headstone's own Phase 5.

## Open questions
1. **ENet vs. GameNetworkingSockets for v0.3.** ENet: one small C library, trivial in premake, no
   encryption. GNS: same API family as the Steam transport (one code path online and offline), but
   pulls in protobuf and OpenSSL/libsodium. Proposed: ENet.
2. **macOS**: the engine doesn't build there yet, but the POSIX socket code would cover it.
   Proposed: keep the code portable, with no macOS package or CI until the engine has one.
3. **Engine roadmap**: update `ROADMAP.md:419-426` to name DingoNet and link its repo once it exists.
4. **v0.2 epoch stamp on every game message** (2 bytes each, unreliable limit 1098). The other way is
   stamping only unreliable messages (reliable ones stay in order), but a client's reliable message
   sent just before it sees a new epoch would then get through stale. Proposed: stamp all, behind
   `SessionParams::Epochs` (off = v0.1 behaviour).
5. **Optional trailing fields** (lesson 5: flags bytes are full, and new fields append in a fixed
   order). A reader could treat missing trailing fields as defaults, but the Hello's protocol and
   content checks already stop mismatched builds from playing. Proposed: not in v0.2.
6. **Headstone's v0.2 adoption waits for its legacy layer to go** (its Phase 5). Proposed: yes;
   DingoNet's phases 1–4 go ahead now.
