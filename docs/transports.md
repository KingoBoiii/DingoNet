# Transports

The `Transport` interface (`Transport.h`), `TcpTransport`, `LoopbackTransport` and `Conditioner`.

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

- Every socket call goes through `src/Platform/Socket.h`; sockets are never inherited by child processes (`SOCK_CLOEXEC`/`accept4` on Linux, `FD_CLOEXEC` elsewhere, `WSA_FLAG_NO_HANDLE_INHERIT` + `SetHandleInformation` on Windows).
- Linux sends use `MSG_NOSIGNAL` (`SO_NOSIGPIPE` where it exists), `EINTR` is retried, `WinSocket.cpp` refcounts `WSAStartup` under a mutex and carries `#pragma comment(lib, "ws2_32.lib")`, so a game linking the static lib names nothing extra.
- Non-blocking connects finish through `PollConnect` (`select` except-set on Windows, `poll` on POSIX, then `SO_ERROR`).

## LoopbackTransport

- `LoopbackTransport::Listen()`/`Connect(host)` and `CreateLoopbackPair()`: in-process queues, many clients per host, delivered at the receiver's next `Poll`, never lost.

## Conditioner

- `Conditioner(std::unique_ptr<Transport>, ConditionerParams)` wraps any transport: lag, jitter (reliable packets never overtake each other), loss of unreliable ones only, its own `mt19937` seeded by `Seed` (99), and an optional `Clock` hook (tests and a game's net clock).
- `Disconnect`/`Close` send what it holds first, and what it holds for a peer the inner transport reports lost is dropped (SteamIDs come back); `Dropped` counts losses.
