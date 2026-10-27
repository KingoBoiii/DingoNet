# Changelog

## v0.2.0

- Released `v0.2.0`, build 40, 2026-10-22.
- Phase 1 message registry (`Messages.h`), phase 2 session epoch (`k_SessionProtocol` 2: not wire-compatible with v0.1 sessions, which a v0.2 host rejects as `ProtocolMismatch`), phase 3 `Mirror.h` interpolation.
- The Opus review (`.claude/reviews/2026-10-22-v0.2-review.md`, F1–F7) and the review of its fixes (R1–R2) are fixed.

## v0.1.1

- Released 2026-10-22 as build 29.
- The Opus review of the whole v0.1 diff (`.claude/reviews/2026-10-22-v0.1-review.md`, 1 High, 2 Medium, 7 Low) and the 5 Low findings of the review of those fixes (R1–R5, same file) are fixed on `master`.
- Wire-compatible with v0.1.0, API additions only: `Session::Event::LastMessages` and `FromBye`.
- Headstone fetches it since PR #133 (2026-10-22).

## v0.1.0

- Released `v0.1.0`, build 11, 2026-10-14.
- Phases 1–5 in this repo; phase 6 merged into Headstone as PR #97 (2026-10-21), behind its premake `--dingonet` option (`HEADSTONE_DINGONET`; the default build keeps Headstone's own net layer in `src/Net/Legacy/`).
- Both of Headstone's gates for making DingoNet its default (its `dingonet-flag-plan.md` Phase 4) passed on 2026-10-21: the owner (Windows) and a friend (Linux build) played a Steam lobby on their own accounts, the first run of the Steam transport and of Windows↔Linux co-op.
- Headstone then deletes the old layer (Phase 5) after one Playtest upload with no net reports.
