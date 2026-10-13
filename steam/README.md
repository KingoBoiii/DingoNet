# Steam transport (source)

`SteamTransport` implements `Dingo::Net::Transport` over `ISteamNetworkingMessages` (channel 0).
It ships as source: the game compiles it against its own Steamworks SDK. DingoNet's build, tests
and CI never touch this folder.

- Add `SteamTransport.cpp` to the game's project, behind a define such as `DN_STEAM`, with the
  SDK's `public/` on the include path and `<DingoNet/...>` from the package.
- Call `SteamAPI_Init()` first, then
  `SteamTransport::Create(SteamNetworkingMessages(), hostSteamId, allowed, log)`:
  `hostSteamId` is the lobby owner for a client and 0 for a host; `allowed(steamId)` says whether
  a user is in the lobby; `log` is optional (route it to your own log).
- Destroy the transport before `SteamAPI_Shutdown()`, or pass `isAlive` (fifth argument, false
  once Steam is down): `Close` then makes no Steam call.
- Lobby code stays in the game. Peers are SteamIDs. Needs C++20. Pump `Poll` once a frame, from
  the thread that runs `SteamAPI_RunCallbacks()`.
