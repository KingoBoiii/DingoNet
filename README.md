# DingoNet

A small cross-platform C++20 netcode library: byte serialization, transports (TCP, loopback, a network conditioner), and a configurable session layer. It is extracted from the Headstone co-op game and is meant to become an optional module for [DingoEngine](https://github.com/KingoBoiii/DingoEngine). The core library is engine-free and logs through a handler the game installs.

## Status

v0.1 (Extract) is in progress. The repo skeleton (build, logging, versioning, test runner) is in place; bytes, transports and sessions come next. See `.claude/plans/2026-10-12-dingonet-plan.md`.

## Platforms

- Windows: MSVC (toolset v145), x86-64
- Linux: GCC 13 or newer, x86-64

## Build

Windows:

```
vendor\premake\bin\premake5.exe vs2026
msbuild DingoNet.slnx /p:Configuration=Debug /p:Platform=x64 /m
build\bin\Debug-windows-x86_64\DingoNet-Tests\DingoNet-Tests.exe
```

Linux:

```
./Generate-Linux.sh
make -j"$(nproc)" config=debug
./build/bin/Debug-linux-x86_64/DingoNet-Tests/DingoNet-Tests
```

Configurations: `Debug`, `Release`, `Distribution`.

## License

MIT, see `LICENSE.txt`.
