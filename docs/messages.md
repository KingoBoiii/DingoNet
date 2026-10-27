# Messages

The message registry (`Messages.h`, `src/Messages.cpp`) and the unreliable size cap.

## Message types

- A game's message is a type with `Id` (integer or enum, 0-255; `0xF0-0xFF` is the session's), `Dir` (`Direction { ToHost, ToClients, Both }`), `Send` (`Delivery`), `Write(ByteWriter&) const`, `bool Read(ByteReader&)` and an optional `Name` for logs (else the hex id); the `NetMessage` concept checks it.
- `Encode(msg)` is the id byte plus `Write` (the wire is exactly that) as an `optional`: an unreliable one over `MAX_UNRELIABLE_BYTES` or a reserved id gives `nullopt` and an error naming it.

## Router

- `MessageRouter::On<T>(handler(slot, const T&))` (a duplicate or reserved id logs an error and returns false; handlers must be copyable).
- `Dispatch(slot, data, isHost)` returns whether a handler ran: an empty packet, unknown id (`DroppedUnknown`), message at the wrong side, short read or trailing bytes is dropped and counted.
- `Stats(id)` has `Received`, `Sent`, `BytesIn`, `BytesOut` (ids included), `DroppedDirection`, `DroppedMalformed` for a net-debug HUD; `KnownIds`, `NameOf`, `Registered`, `ResetStats`.
- `Sent` moves only through `NoteSent<T>`, which the session's typed sends call once a packet is handed to the transport.
- The table is heap-allocated once (a handler may call `On`/`Dispatch`); the router is neither copyable nor movable, since a `Session` holds a pointer to it.

## Unreliable size cap

- `MAX_UNRELIABLE_BYTES = 1100` (Steam drops a whole fragmented unreliable message); `Send` logs an error and drops a larger one; it applies to `Unreliable` and `UnreliableSequenced`.
- A session with epochs on allows `Session::MaxUnreliablePayload()` = 1098 (the stamp), checked by the session itself in both modes (an oversize span `Send` returns false).
