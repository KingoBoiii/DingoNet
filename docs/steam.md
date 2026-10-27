# Steam

The Steam transport source drop (`steam/`) and the other third-party links.

## Linking

- The core links only the OS: Winsock (`ws2_32`, pulled in by `#pragma comment` in `WinSocket.cpp`) on Windows, libc on Linux.
- Steam ships as source (`steam/`) that the game compiles against its own Steamworks SDK.

## SteamTransport

- `SteamTransport::Create(ISteamNetworkingMessages*, host, allowed, log, isAlive)` on channel 0, with the stale-queue drain on create.
- `log` is a callback because the package exposes no log function, and `isAlive` (or destroying it before `SteamAPI_Shutdown()`) keeps every call off a dead interface (`Create` returns null, `Send`/`Poll` do nothing, `Close` closes no session; its callbacks still unregister in the destructor).
- After `Close` the transport is inert, like TCP's: `Send`, `Poll`, `Disconnect` and the session-request callback do nothing.

## Failures and rejoin

- Remote, local and connectivity failures are `ConnectFailed` (Steam has no explicit deny, so nothing is `Refused`).
- A disconnected peer's messages are dropped for 1 s (long enough for in-flight packets) unless one is a session `Hello` (first byte `0xF0`, only ever a new session's), which ends the window so an instant rejoin after a kick or `Bye(TimedOut)` gets in.
- Failed sends are logged.
