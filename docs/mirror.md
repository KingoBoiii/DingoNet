# Mirror

The client-side interpolation helpers in `Mirror.h`: `RenderClock`, `Tracks`, `Timeline` and `EventQueue`.

## Basics

- `Mirror.h` is header-only, with no clock read and no thread; times are doubles in seconds the caller passes.
- The game supplies `Interpolate(a, b, t)` and optionally `Extrapolate(a, b, t>1)`, found by ADL (`double` overloads are built in, ints truncate through them; a third-party type like `glm::vec3` needs a wrapper or functions in an associated namespace; a mistyped `Extrapolate` silently holds, so `static_assert(Tracks<K, T>::Extrapolates)`).

## RenderClock

- `RenderClock(RenderClockParams{Delay 0.1, MaxSlew 0.1, CatchUp 0.25, SnapError 0.5, MaxDt 0.25, RewindError 2.0, RestartError 2.0})`.
- `OnSnapshot(hostStamp)` per snapshot, `Advance(localDt)` per frame (dt clamped to `MaxDt`).
- `HostNow()` (never behind the latest stamp; eases down at most `MaxSlew` when ahead of latest + time since arrival, but drops at once to a newer stamp more than `SnapError` behind it, a resync).
- `RenderTime()` (`Delay` behind, rate `1 ± MaxSlew`, snaps forward only when more than `SnapError` behind; ahead of its target by more than the slew band (`MaxSlew * CatchUp`, 0.025 s) it holds, rate 0, until the target catches up, so network delay whose backlog arrives over several frames never moves it back).
- `HasTime`, `LatestStamp`.
- `Settle()` (render moves forward to the latest stamp and stops, or holds if already past it, never backwards; `HostNow` held at the latest stamp, until a newer stamp; Game Over).
- `Reset()`, `Restarts()`.

### Moving render time back

- Two cases move render time back, on purpose: render ahead of its target by more than `RewindError` (a host stall; keep it at most `Tracks`' `Stale`) rewinds to the target, and a stamp more than `RestartError` behind the latest (host restart) restarts the clock and counts; an out-of-order stamp closer than that is ignored.
- While held, or after a `HostNow` resync, `RenderTime()` can be ahead of `HostNow()`: do not assume `HostNow() - RenderTime() >= 0`.

## Tracks

- `Tracks<Key, T>(TracksParams{MaxExtrapolate 0.15, Stale 2.0, History 1.0, MinExtrapolateGap 0.001})`.
- `Push(key, hostTime, value)` (kept in time order, a duplicate time replaces).
- `Sample(key, renderTime)` → `optional<T>` (interpolates; past the newest it extrapolates from the newest and the latest earlier sample at least `MinExtrapolateGap` back, up to `MaxExtrapolate`, then holds; none to extrapolate from, or no `Extrapolate`, holds; before the oldest holds it).
- Explicit `Trim(renderTime)` once a frame (forgets keys whose newest sample is older than `Stale`, drops samples older than `History` but keeps the bracket).
- `Has`, `Count`, `SampleCount`, `Keys()` (allocates), `ForEach(fn(key))` (unspecified order), `Forget`, `Clear`.

## Timeline and EventQueue

- `Timeline<T>`: `Set(hostTime, value)`, `At(renderTime)` → latest value at or before it, `Trim`.
- `EventQueue<E>`: `Schedule(hostTime, e)`, `TakeDue(renderTime)` (time order, insertion order for equal times, once each; a past event is due at the next call; drains only here).
- Push/Set/Schedule grow until `Trim`/`TakeDue`.

## Eras

- None of them knows an era: the game clears Tracks, Timeline and EventQueue and calls `RenderClock::Reset()` at an epoch change or run start (`BeginEpoch` on the host, `EpochChanged` on clients).
