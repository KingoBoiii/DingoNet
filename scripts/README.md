# Scripts

What `scripts/windows/*.bat` and `scripts/linux/*.sh` do, and the platform notes behind them.

## Scripts

- `Generate`: premake (`vs2026` on Windows, `gmake` on Linux); extra arguments pass through to premake.
- `Build`: generates first if the solution or Makefile is missing, then MSBuild on `DingoNet.slnx` (found through `vswhere`) or `make -j"$(nproc)"`.
- `Test`: `Build`, then runs `DingoNet-Tests`; the exit code is the runner's.
- A configuration argument is optional (`Debug` by default; `Release`, `Distribution`; Linux also takes lower case).

## Package

- `Package [Config...]` (default all three): builds each and writes `build/package/DingoNet-<DN_VERSION>.<build>-<Config>-<os>-x86_64.zip` with `include/`, `lib/<Config>/{DingoNet.lib | libDingoNet.a}`, `steam/`, `LICENSE.txt`, `THIRD_PARTY.txt` (ENet's license), `VERSION.txt`; no top-level folder, forward-slash entries (Windows zips through `tar.exe`, Linux through `zip` or `python3 -m zipfile`).
- `DN_VERSION` (MAJOR.MINOR.PATCH, default `<VERSION>.0`), `DN_BUILD_NUMBER` (default the commit count), `DN_NO_BUILD=1` (`.bat` only: skip building, for CI).
- It stamps `BuildInfo.h` for the build and restores it with a fresh timestamp, so later builds recompile `Version.cpp`.

## Pausing

- The `.bat` files pause only for an Explorer double-click: no arguments, `%CMDCMDLINE%` holds their full path, and the `cmd.exe` running them has `explorer.exe` as its parent (asked through PowerShell, since PowerShell's `& .\Build.bat` gives cmd the same command line).
- A script calling one by full path passes a config, so shells, PowerShell, scripts and CI never hang on them.

## Steam tests

- `Generate --steam-sdk=<SDK root>` (the folder holding `public/`) adds `DingoNet-SteamTests`: `steam/SteamTransport.cpp` against the SDK's headers, a fake `ISteamNetworkingMessages` and stand-ins for steam_api's callback registration (`tests/steam/`), so no Steam and no SDK library.
- Run its exe by hand; CI and `Test` don't build it.

## Windows

- MSVC, toolset v145: `staticruntime "Off"` and v145 match Headstone's, so a game links the engine and DingoNet packages with one CRT.
- On LNK1318, build with `/m:1`.

## Linux

- GCC >= 13, x86-64: tested locally in WSL2 (`Ubuntu-24.04`, repo at `/mnt/d/Git/Github/KingoBoiii/DingoNet`).
- `/mnt/d` is case-insensitive, so a wrong-cased include still builds there; CI on `ubuntu-latest` catches it.
- The library is built `pic "On"` (`-fPIC`), so it links into a shared object too; make doesn't notice changed flags, so rebuild a Linux tree clean after a premake flag change.

## CI

- `build.yml` builds and tests Debug, Release and Distribution on `windows-latest` and `ubuntu-latest` on every push and PR to master.
- `release.yml` runs when a GitHub release is published (its tag must be `vX.Y.Z`) or on a manual dispatch with a `tag` input, checks the tag against `VERSION`, packages all three configs on both OSes and uploads them with `gh release upload --clobber`, creating the release if it is missing.
- Windows CI builds with `/p:PlatformToolset=v143` (`WINDOWS_TOOLSET`), as the engine's does, since the runners lack v145; a v143 static lib links into a v145 game.
