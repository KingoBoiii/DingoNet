# LAN discovery

`LanHost` and `LanBrowser` (`LanDiscovery.h`, `src/LanDiscovery.cpp`): a game finds a host on the local network without typing an IP. Raw UDP through `src/Platform/Socket.h` (`OpenUdpSocket`, `SetBroadcast`, `SendTo`, `RecvFrom`), IPv4, no ENet, no threads: both are pumped by `Poll()` once a frame.

## Protocol

- **Query/response**, not a beacon: nothing is sent while no one browses.
- A browser binds an ephemeral port, so several browsers on one machine coexist, and sends a query to `QueryAddress:Port` (default `255.255.255.255:27961`) on its first `Poll` and then every `QueryInterval` (1 s).
- A host bound to the discovery port answers each query by unicast to its source.
- Header: `DNLAN`, type (1 query, 2 reply), discovery version (1).
- **Query**: header, game `Protocol` (u16), zero padding to 512 bytes. A host ignores the padding's content and size beyond the minimum: it needs at least the size of its reply.
- **Reply**: header, `Protocol` (u16), `ContentHash` (u32), game port (u16), name (`Str`, at most 64 bytes, after the `NameFilter`), seats taken and max (u8 each, max 1-32), accepting (0 or 1), game data length (u16) and up to `LAN_MAX_GAME_DATA` (256) bytes. At most 342 bytes.
- **No amplification**: a host answers only a query at least as large as the reply it would send, so a spoofed source never gets back more than it sent.
- **Strict decode** on both sides (a query's padding aside): wrong magic, type, version, short or trailing bytes, out-of-range seats, accepting not 0/1 or a game data length over 256 is dropped and counted (`Malformed()`).

## LanHost

- `LanHost::Open(params, error)` (`LanHostParams`: `Port` (0 picks one, `GetPort()`), `BindAddress` ("0.0.0.0"), `Protocol`, `ContentHash`, `GamePort`, `Name`, `NameFilter`). A busy port fails with an error (no port sharing).
- `SetStatus(LanHostStatus{SeatsTaken, SeatsMax, Accepting, GameData})` whenever it changes; the next query sees it.
- `Poll()` reads at most 256 datagrams and answers at most 32 queries; the rest are dropped (`Throttled()`). `Answered()`, `Malformed()`.

## LanBrowser

- `LanBrowser::Open(params, error)` (`LanBrowserParams`: `Port`, `QueryAddress`, `Protocol`, `ContentHash`, `QueryInterval` (at least 0.1 s), `HostTimeout` (3 s, at least the interval), `NameFilter`, `Clock`).
- `Hosts()` lists `LanHostInfo` in first-seen order: `Address`, `GamePort` (what a joiner connects to), `Source` (the answering socket, "ip:port"), `Name`, `Protocol`, `ContentHash`, seats, `Accepting`, `GameData`, `ProtocolMatches`, `ContentMatches`. A mismatched host is listed, so a game can grey it out.
- A host is keyed by its answering socket, refreshed by each reply, and dropped after `HostTimeout` of silence. At most `LAN_MAX_HOSTS` (64) are kept; further new ones are ignored.
- Names are filtered on arrival (`NameFilter`, else `DefaultNameFilter`).

## Platform notes

- Windows turns off `SIO_UDP_CONNRESET`, or an ICMP port-unreachable for an earlier query makes a later `recvfrom` fail with `WSAECONNRESET`.
- Replies are unauthenticated: a peer on the LAN can fill the 64-entry list with fake sources or forge an entry. Accepted for LAN play; a game still validates in the session handshake.
- A custom `NameFilter` runs on raw bytes and the result is cut at 64 bytes, which can split a multi-byte character.
- A broadcast goes out the default adapter only (a VPN or second NIC can swallow it: check by hand on the real LAN). WSL2 in NAT mode does not pass broadcasts: the real-LAN check is by hand (mirrored networking or two machines).
- Tests address the host directly (`QueryAddress` "127.0.0.1"), since CI runners cannot be relied on for broadcast.
