# DingoNet

A small cross-platform C++20 netcode library: byte serialization, transports (TCP, loopback, a network conditioner), and a configurable session layer. It is extracted from the Headstone co-op game and is meant to become an optional module for [DingoEngine](https://github.com/KingoBoiii/DingoEngine). The core library is engine-free and logs through a handler the game installs.

## Status

v0.1 (Extract) is in progress. The repo skeleton (build, logging, versioning, test runner) is in place; bytes, transports and sessions come next. See `.claude/plans/2026-10-12-dingonet-plan.md`.

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

## License

MIT, see `LICENSE.txt`.
