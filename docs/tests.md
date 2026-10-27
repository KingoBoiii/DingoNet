# Tests

How the `DingoNet-Tests` cases are built and which file covers which area.

## Rigs

- Network tests run over the loopback transport (a `Conditioner` with a fake clock for lag), or over real sockets on `127.0.0.1` port 0, each poll loop capped by time (`TransportTestUtils.h`: `PumpUntil`, `LogCapture`) (on Windows a refused connect waits out SYN retries).
- `tests/MessageTests.cpp` (the registry), `tests/EpochTests.cpp` (epochs over a lagged loopback rig) and `tests/MirrorTests.cpp` (`RenderClock`, `Tracks`, `Timeline`, `EventQueue`, a simulated 20 Hz feed) cover v0.2.
- `tests/UdpTests.cpp` covers `UdpTransport` (every delivery, caps, timeouts, a 10 % datagram-loss hook, sessions over UDP, a lagged `Conditioner` stream). The timing tests have wide limits or a retry because WSL2's wall clock jumps and ENet reads it.
- `tests/LanDiscoveryTests.cpp` covers `LanHost` and `LanBrowser` over real UDP sockets on `127.0.0.1` (a host addressed directly, raw datagrams for the malformed, size-bound and rate-limit cases, a fake clock for query and expiry timing).
- Session tests (`tests/SessionTests.cpp`) drive time through `SessionParams::Clock` and pump a fixed number of rounds, with a few runs over TCP (a handshake, a full 32-seat session, 70 idle sockets) and a `FakeSteam` transport for Steam's semantics (silent `Disconnect`, a 1 s ignore window, a returning SteamID as a new connection).

## Cross-OS check

- A cross-OS check closes each transport phase: `DingoNet-Tests --tcp-host=<ip>:<port>` on one machine, `--tcp-client=<ip>:<port>` on the other (`--udp-host=` and `--udp-client=` for `UdpTransport`) (`tests/NetCheck.cpp`, 200 echoed packets).
- From WSL2 in NAT mode a Windows host needs an inbound firewall rule on the `vEthernet (WSL (Hyper-V firewall))` interface (or mirrored networking); the reverse, a WSL2 host on `0.0.0.0` and a Windows client on `127.0.0.1`, needs nothing for TCP; for UDP the client must use the WSL2 address (`wsl hostname -I`), since localhost forwarding carries TCP only.
