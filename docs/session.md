# Session

The session layer (`Session.h`, `src/Session.cpp`, wire in `src/SessionMessages.{h,cpp}`): handshake, seats, events, pings, timeouts and epochs.

## API

- Owns a `Transport`; `StartHost`/`StartClient(transport, name)`, `Update()` once a frame (time from `steady_clock`, or `SessionParams::Clock`), `TakeEvents`/`TakeMessages`, `Stop` (a `Bye` to everyone, no event; the destructor calls it), host `Kick(slot)` and `SetAccepting`.
- The span `Send` returns whether it reached the transport and `Broadcast` how many seats.
- The typed `Send(slot, msg)` / `Broadcast(msg, exceptSlot)` use the message's declared delivery, drop a `ToHost` message sent by a host or a `ToClients` one sent by a client (error), and, with `SetRouter(MessageRouter*)` (not owned), count `Sent` only for what was handed over (one per `Broadcast` that reached a seat).
- The session never dispatches: the game feeds `TakeMessages` to `router.Dispatch`.

## Handshake and seats

- Slot 0 is the host; `SessionParams::MaxSlots` (1–32, clamped with a warning) sizes the seats; a client adopts a host's smaller count from the `Welcome` and turns a larger one away (`Rejected(ProtocolMismatch)`, then a `Bye`), so no event ever names a slot past the game's own arrays.
- The first byte is the message id: games own `0x00–0xEF`, the session `0xF0–0xFF` (`Hello` F0, `Welcome` F1, `Reject` F2, `Ping` F3, `Pong` F4, `Bye` F5, `SeatJoined` F6, `SeatLeft` F7, `Epoch` F8, `Stamped` F9); `Send`/`Broadcast` of a reserved id logs an error and drops it, and an empty packet is the game's.
- `Hello` = `k_SessionProtocol` (2; bump it whenever these messages change) + the game's `Protocol` + `ContentHash` + name + `Build`; the host checks session protocol, then `Protocol` (both `ProtocolMismatch`), `ContentHash` (`ContentMismatch`), `SetAccepting` (`NotAccepting`), a free seat (`Full`).
- Hello's leading session protocol and the whole `Reject` (reason, the host's versions and build) never change layout, so two session protocols can still turn each other away.
- A `Hello` from a seated peer (a Steam peer rejoining under the same SteamID after a session that ended without a `Bye`) frees its seat (`Left(TransportLost)`, `Transport` `Reset`) and then counts as a fresh join; a client's `Lose` sends no `Bye`, because the host's 1 s Steam ignore window would swallow an immediate rejoin's `Hello`.
- After each `Update`'s packets (so a joiner whose `Hello` came in that `Poll` is already seated) a host trims the connections waiting for a `Hello` to 16, letting go first of those that never sent a byte, oldest first, so idle sockets or a connection flood can't hold a transport's cap against joiners.
- `NameFilter` (null = `DefaultNameFilter`: printable ASCII, trimmed, 16 chars, "Player" if empty) runs on every name the session takes in, its own included.

## Events and reasons

- Events: `Joined`/`Left` on the host and, through `Welcome`'s roster and `SeatJoined`/`SeatLeft`, on every client for the other clients (so `SlotTaken`/`NameOf` are right everywhere; roster joins carry `Existing`, so a game doesn't announce players already there).
- `Welcomed` (Name = the host's), `Rejected` (client: the session stopped; host: who it turned away), `Lost` (client: stopped; `Slot` -1 if never seated; `FromBye` when the reason came in the host's `Bye`, so a game tells `Lost(TimedOut)` "the host stopped hearing us" (`FromBye`) from "we stopped hearing the host" (not)).
- Codes: `RejectReason { ProtocolMismatch, ContentMismatch, NotAccepting, Full }`, `LeaveReason { Bye, TimedOut, Kicked, TransportLost }`.
- With `TransportLost`, `Event::Transport` holds our own transport's `DisconnectReason`: a host's `Left`, a client's `Lost`; a client's `Left` for another client holds the reason the host's transport reported, carried in `SeatLeft` (`Closed` unless `TransportLost`; any other combination is malformed).
- Plus a log-only `Detail` (the transport's words, or both sides' versions on a reject, a peer's build cut to 64 printable ASCII).
- A slot's game messages never reach `TakeMessages` after its `Left`: those still queued when it goes move to the `Left`'s `LastMessages` (a parting message before a `Stop` lands there), so none is read as the next occupant's.

## Pings and timeouts

- Ping every `PingInterval` (1 s, clamped to `Timeout / 2`; unreliable, u32 ms since start echoed back; a host answers only seated peers) → `RttMs(slot)`, smoothed 0.3 from the first sample (a client pings only the host, so only its slot 0 has one).
- A seated peer silent for `Timeout` (5 s) is dropped, a connection without a `Hello`/`Welcome` after `HandshakeTimeout` (5 s; a client's runs from `StartClient` and restarts at the transport's `Connected`) too, a seated one after a `Bye(TimedOut)` from the host (a Steam `Disconnect` is silent, and a timed-out client would otherwise come back as a new connection believing it still had its seat); an unseated one gets no `Bye`, so a v0.1.0 client that came back unseated is never kept alive by them.
- A `Bye` may carry `Bye`, `Kicked` or `TimedOut` (v0.1.0 reads `TimedOut` as malformed and times out on its own).

## Epochs

- `SessionParams::Epochs` (host's setting; a client adopts it and the current epoch from `Welcome`): every game message, reliable or not, goes as `Stamped` (`F9 epoch game...`, the game sees its own bytes), and a stamp that is not the receiver's current epoch is dropped silently (`StaleDropped()`, never toward the 32-bad hang-up).
- Host `BeginEpoch()` (warning and no-op on a client or with epochs off) bumps the `u8` (wraps), sends `Epoch` (F8) reliably, and drops the queued messages and the pending `LastMessages`; a client does the same on `Epoch` and raises `EpochChanged` (`Event::Epoch`); `GetEpoch()`.
- Order: call `BeginEpoch()` first, then send the new state's messages (a `RunOver` that must arrive goes after it, in the new epoch); a message sent before it belongs to the old epoch and may be dropped, even one already received but not yet taken.
- A game takes events before messages each frame (`TakeEvents`, then `TakeMessages`), so it clears its mirror on `EpochChanged` before the new epoch's first state.
- `EpochsOn()` and `MaxUnreliablePayload()` follow one flag: the params' `Epochs` before `Start` and after `Stop`/`Lost`, the host's (a client's from `Welcome`) while started.
- Source compatibility: `Event::Kind::EpochChanged` is new, so a v0.1 game's exhaustive `switch` over `Event::Kind` needs a case or a `default` (`-Wswitch` under `-Werror` on GCC and clang).
- Unstamped game packets from a seated peer with epochs on, a `Stamped` with epochs off or from an unseated peer, and an empty send with epochs on are refused; raw packets from unseated peers still reach `UnseatedPacket`.

## Decoding and hang-ups

- Every decode is strict (short read, trailing bytes, out-of-range enum or slot = malformed): malformed, unexpected-side and unknown session packets are dropped, never fatal, with one warning per peer, problem and id (the rest counted); a host hangs up on a peer after 32 (`Bye(Kicked)`).
- Game packets from an unseated peer go to the host's `SessionParams::UnseatedPacket` hook, which may return a raw reply that is sent before the hang-up (how a game answers an older client's Hello; without it that client only times out), else are dropped.

## Update order

- Order in `Update`: the transport's events in order, except that a plain disconnect waits for the packets (a `Bye` that arrives with the hang-up reads as `Bye`) and a disconnect followed by a connect of the same id (a SteamID back) drops the old peer first; then packets (one from an untracked peer makes the host `Disconnect` it), deferred disconnects, pings, timeouts.
