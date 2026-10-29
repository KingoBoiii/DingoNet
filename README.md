# DingoNet

A small cross-platform C++20 netcode library: byte serialization, transports (TCP, UDP on ENet, loopback, a network conditioner, Steam as source), LAN discovery, a configurable session layer, a typed message registry, session epochs and snapshot interpolation. It is extracted from the Headstone co-op game and is meant to become an optional module for [DingoEngine](https://github.com/KingoBoiii/DingoEngine). The core library is engine-free and logs through a handler the game installs.

## Status

v0.1 (Extract) is released (`v0.1.1`). v0.2 (Structure) is released (`v0.2.0`): it adds `Messages.h` (typed messages and a `MessageRouter` with per-message stats), session epochs (`SessionParams::Epochs`, `BeginEpoch`; session protocol 2, so v0.2 and v0.1 sessions turn each other away) and `Mirror.h` (`RenderClock`, `Tracks`, `Timeline`, `EventQueue`). v0.3 (UDP) is released (`v0.3.0`): `UdpTransport` on bundled ENet (reliable ordered, unreliable and `Delivery::UnreliableSequenced` channels; docs in `docs/transports.md`) and LAN discovery, `LanHost` and `LanBrowser`, so a game lists hosts without typing an IP (`docs/discovery.md`). The session wire is unchanged (session protocol 2), so v0.3 and v0.2 games still play together on TCP or Steam. See `.claude/plans/2026-10-12-dingonet-plan.md`.

## Platforms

- Windows: MSVC (toolset v145), x86-64
- Linux: GCC 13 or newer, x86-64

## Build

Each platform has the same three scripts, runnable from any directory, with an optional configuration (`Debug` by default, `Release`, `Distribution`):

| Script | Windows | Linux |
|---|---|---|
| Generate project files | `scripts\windows\Generate.bat` | `scripts/linux/Generate.sh` |
| Build | `scripts\windows\Build.bat Release` | `scripts/linux/Build.sh Release` |
| Build and run the tests | `scripts\windows\Test.bat` | `scripts/linux/Test.sh` |

Windows needs Visual Studio with the C++ workload (toolset v145); Linux needs GCC 13 or newer and `make`.

## Bundled libraries

[ENet](https://github.com/lsalzman/enet) 1.3.18 (MIT) is compiled into `DingoNet.lib` for `UdpTransport`, so a game links one library and names nothing extra. A game that links its own ENet gets duplicate symbols and must drop its copy. Its license ships in the package as `THIRD_PARTY.txt`.

## License

MIT, see `LICENSE.txt`.
