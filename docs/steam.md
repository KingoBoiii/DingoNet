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
- `Disconnect` leaves the Steam session open (closing it could drop a parting `Bye`), and so does a client's `Stop`, so a player who leaves and rejoins within Steam's idle timeout (3 minutes after the last send) reuses that session. No new session handshake happens, so the rejoin's `Hello` gets to the host at once, often before the host's copy of the lobby lists the player again (issue #3).
- So a host holds, rather than drops, what comes from users `allowed` doesn't yet say yes to, for 3 s, and asks `allowed` again on each `Poll`:
  - Messages from a `Hello` on (before a `Hello` they are a departed session's tail and are dropped as before): up to 32 messages and 64 KB per user, for up to 16 users. Only a user whose session was once accepted can send any. On a yes they come first, in order, with the `Connected` (logged at Info); after 3 s they are dropped (logged at Info). A `Hello` that finds all 16 slots taken is dropped with a warning.
  - Session requests, up to 64 users, kept apart from the messages, because anyone on Steam can send one. On a yes the session is accepted; after 3 s, or with no room, it is refused with a warning.
  - A failed session (`SessionFailed`) drops whatever it had held, and so does `Disconnect`.
  - A host with no `allowed` holds nothing: nothing could ever say yes.
- A v0.3.0 host still drops that `Hello`, whichever version the client runs.
- Failed sends are logged.
