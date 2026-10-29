# Transports

The `Transport` interface (`Transport.h`), `TcpTransport`, `UdpTransport`, `LoopbackTransport` and `Conditioner`.

## Interface

- `Send(PeerId, std::span<const uint8_t>, Delivery)`, `Poll(packets, events)` once a frame (flushes, then appends arrivals and `PeerEvent`s), `Disconnect`, `Close`, `Describe`, public `BytesIn`/`BytesOut`.
- A disconnect is a `DisconnectReason` (`Closed`, `TimedOut`, `Refused`, `Reset`, `ConnectFailed`, `Error`) plus a `Detail` string for logs only; a peer we `Disconnect` raises no event on our side, and what we sent before it still arrives.

## TcpTransport

- `TcpTransport::Listen/Connect(address, port, error, TcpParams)` (IPv4; port 0 picks one, `GetPort()`).
- Caps: 64 KB frame, 4 MB send queue checked after each flush, 64 live connections (lingering ones don't count; room for a full session's 31 clients plus the 16 handshakes a session host keeps; it also caps the sockets lingering after our `Disconnect`, past which the one lingering longest is closed at once), 256 KB read per connection per `Poll` so a flooding peer can't stall a frame, 5 s connect, 2 s close linger.
- Frames are a 4-byte little-endian length + payload, wire-compatible with Headstone; a reliable packet over the frame cap is logged and dropped at the sender, a bigger incoming frame or a full send queue drops the peer (`Error`).
- A connect that fails at once still reports through `Poll`.

### Poll, Disconnect and Close

- **Read before send** in `Poll` on both OSes (a send to a peer that has closed brings back a reset, and Windows then discards unread data, a parting `Bye` with it).
- `Disconnect` keeps flushing the whole queue in later `Poll`s, shuts our side only once it is empty, then drains until their FIN (all within `CloseLinger`).
- `Close` flushes for at most 0.25 s (`WaitWritable`), then hangs up.

### Sockets

- Every socket call except ENet's own (see `UdpTransport`) goes through `src/Platform/Socket.h`; sockets are never inherited by child processes (`SOCK_CLOEXEC`/`accept4` on Linux, `FD_CLOEXEC` elsewhere, `WSA_FLAG_NO_HANDLE_INHERIT` + `SetHandleInformation` on Windows).
- Linux sends use `MSG_NOSIGNAL` (`SO_NOSIGPIPE` where it exists), `EINTR` is retried, `WinSocket.cpp` refcounts `WSAStartup` under a mutex and carries `#pragma comment(lib, "ws2_32.lib")`, so a game linking the static lib names nothing extra.
- Non-blocking connects finish through `PollConnect` (`select` except-set on Windows, `poll` on POSIX, then `SO_ERROR`).

## LoopbackTransport

- `LoopbackTransport::Listen()`/`Connect(host)` and `CreateLoopbackPair()`: in-process queues, many clients per host, delivered at the receiver's next `Poll`, never lost.

## Conditioner

- `Conditioner(std::unique_ptr<Transport>, ConditionerParams)` wraps any transport: lag, jitter (reliable packets never overtake each other), loss of unreliable ones only (both kinds), its own `mt19937` seeded by `Seed` (99), and an optional `Clock` hook (tests and a game's net clock).
- `Disconnect`/`Close` send what it holds first, and what it holds for a peer the inner transport reports lost is dropped (SteamIDs come back); `Dropped` counts losses, and an `UnreliableSequenced` packet jitter would release after a newer one to the same peer is dropped and counted too.

## Delivery::UnreliableSequenced

- `IsUnreliable(d)` is true for it and `Unreliable`: never delivered after a newer packet from the same peer.
- Only `UdpTransport` enforces it; TCP, loopback and Steam treat it as `Unreliable`. The 1100-byte cap applies to both.
- Source compatibility: it is a new `Delivery` value, so a game's exhaustive `switch` over `Delivery` needs a case or a `default`.

## UdpTransport

- `UdpTransport::Listen/Connect(address, port, error, UdpParams)` (`UdpTransport.h`, `src/Platform/UdpTransport.cpp`, on the bundled ENet; IPv4, port 0 picks one, `GetPort()`).
- Three ENet channels (0 reliable, 1 `Unreliable` as unsequenced, 2 `UnreliableSequenced`). No ENet type in any public header; `enet.h` only in that file (defined `NOMINMAX`/`WIN32_LEAN_AND_MEAN` first). A refcounted `enet_initialize` under a mutex; `PeerId` is a counter, never the `ENetPeer*`.
- Caps in `UdpParams`: 64 KB reliable packet (ENet fragments and reassembles it; a larger one is logged and dropped at the sender, and one that arrives anyway, from a peer with a bigger limit, is logged and drops that peer with `Error`, the remote seeing `Closed`; ENet's own incoming cap is set above the limit, since a datagram it refused would go unacked and the sender would only time out), 4 MB queued and unacked per peer (`MaxQueuedBytes`, at least the reliable cap: each packet's ENet `freeCallback` gives its bytes back, and a peer past the cap is logged and dropped with `Error` at the next `Poll`, as TCP does with a stalled reader), 64 live peers (a peer we hung up on keeps its slot until its end acks or 2 s pass), 32 per IP address (`MaxConnectionsPerAddress`, ENet's `duplicatePeers`, connecting ones included), at most 16 connects that have raised no event (the oldest past 16, and any older than `ConnectTimeout`, are `enet_peer_reset`, so half-open clients cannot hold every slot), 256 KB read per `Poll` (each event charged 64 bytes on top of its payload, so empty packets cannot dodge the budget), 5 s `ConnectTimeout`, 8 s `PeerTimeout` (above the session's 5 s, so the session times out first; `enet_peer_timeout` with min = max, since the 500 ms initial RTO makes ENet's own limit coarse).
- A remote hang-up carries data 1 (`Closed`), data 0 on a disconnect event is `TimedOut`, a peer that never reached CONNECT is `ConnectFailed`; our own `Disconnect` raises no event (`disconnect_later`, so the queue still goes out).
- `Close` services for at most 0.25 s so the hang-up and what was queued leave, which needs the peer to be polling: acks cannot arrive otherwise, the hang-up goes out unsequenced and the data may be lost (seen in a single-threaded test: a parting `Bye` gone, so `Left(TransportLost)` instead of `Left(Bye)`).
- ENet's service fails (returns -1) on any failed `sendto` or `recvfrom`, which includes a send to one unroutable address; on Windows an ICMP port unreachable would also fail the next receive, so the socket gets `SIO_UDP_CONNRESET` off (`DisableUdpConnReset` in `Socket.h`, a no-op on POSIX). A failed call is counted and `Poll` carries on (at most 4 failed calls per `Poll`), with one warning per 10 s carrying the count and the OS error (`CurrentNativeSocketError`); a peer that stays unreachable is left to ENet's peer timeout. Only when every call has failed for 1 s does the transport raise `Error` for every peer (`ConnectFailed` for one not yet connected) and close, inert like after `Close`.
- A failed `enet_peer_send` is warned once per peer. A peer with fewer ENet channels than DingoNet's three is refused at CONNECT: a host disconnects it at once with one warning and raises no event; a client raises `ConnectFailed`.
- A `PeerRecord` is freed only after its ENet peer is reset (`enet_peer_disconnect_now` first), since a queued packet's `userData` points at it.
- A host that is full never answers (the client gets `ConnectFailed`); a session host trims unseated connections to 16, so a test joins its 31 clients one by one.
- ENet raises the Windows timer resolution (`timeBeginPeriod`) while a transport is open. On Linux ENet reads the wall clock (`gettimeofday`), so a jumping clock (WSL2) can fire its timeouts early.
- `src/Internal/UdpTestAccess.h` (tests only) drops a seeded share of the datagrams a transport receives through ENet's `intercept` callback: a `Conditioner` cannot lose a reliable packet, this loses the datagrams under it. The same hook fails the next n receives (`FailIncomingDatagrams`); `BreakSocket` closes ENet's socket under a transport; `IsOpen`, `PendingPeers` and `QueuedBytes` read its state; `Open(..., channels, ...)` makes a transport with fewer ENet channels, as a foreign end would.
