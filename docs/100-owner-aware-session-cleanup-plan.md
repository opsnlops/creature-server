# Issue #100 — Owner-aware SessionManager queue and playlist cleanup

**Status:** Implemented (including code-review fixes below)
**Issue:** [#100](https://github.com/opsnlops/creature-server/issues/100)

## Post-review hardening (2026-08-21)

A high-effort adversarial review of the first implementation confirmed six
correctness gaps, all fixed:

- **Chains die on more paths than load failure.** Every cancellation site
  (conflicting/interrupt adoption in `registerSession`, creature-targeted
  takeover in `cancelSessionsForCreatures`, `stopPlaylist`'s sweep) now purges
  the cancelled session's chain entries in the same critical section — except
  when the replacing session continues the same chain. The onFinish next-hop
  schedule failure calls a new `dropChainEntries()`. Without this, a dead
  chain's entries blocked the queue forever and leaked the universe state.
- **`queueAnimation` returns whether the entry was accepted**, and
  `StreamingAdHocSession` falls back to scheduling the sentence directly when
  its chain went quiet (a short sentence finishing before the next render
  resolves is routine) — speech plays immediately instead of being silently
  dropped, and sentence outcomes are now recorded honestly.
- **Pops match the finisher's earliest entry** (`find_if`), not only the deque
  front, so a foreign entry can't block a live chain.
- **The interrupt-owner token exists before the async load is submitted**:
  `interrupt()` mints a token (the chain id, or a one-off UUID that also
  becomes the session's chain id) and stamps it in the same locked section
  that marks the playlist Interrupted. Stamping after `scheduleAnimation`
  returned lost a race against fast load failures — re-creating the exact
  stranded-Interrupted bug this issue fixes.
- **A second interrupt over a pending one takes over the whole resolution** —
  token *and* resume decision — and the failure rollback restores the previous
  owner's, guarded on the token still being ours (a post-adoption commit
  failure has already resolved the playlist via the abort path, and that
  resolution stands).
- **`clearPlaylist` keeps the universe state alive while chain entries are
  queued**, matching the abort path's guard.

Cleanups from the same review: the write-only `playlistOwnerGeneration` field
was deleted (session ids are unique; the id match is exact), the playlist
reset is one `clearPlaylistStateLocked()` helper, and the unwind's
resume/stop broadcast is a single branch sharing the onFinish path's
empty-playlist guard.

## Problem

`SessionManager` stores one `UniverseState` per universe: a *vector* of active
sessions (non-conflicting sessions — disjoint creature/fixture sets — coexist),
but exactly **one** `animationQueue` and **one** playlist state, neither of
which records who owns it (`SessionManager.h:296-314`). Consequences:

1. `abortLoadingSession` (`SessionManager.cpp:699-741`) drops **every** queued
   animation when any session's load fails (`:722-726`), and clears the whole
   playlist snapshot whenever the failing session's reason is `Playlist`
   (`:728-734`) — even when a different, still-live session owns that state.
   `unwindAdoptedAudioSession` then broadcasts an **empty** playlist status to
   all clients (`CooperativeAnimationScheduler.cpp:163-170`).
2. Any finishing session on a universe pops the chained animation queue
   (`CooperativeAnimationScheduler.cpp:924`), including sessions that never
   enqueued anything — so a bystander session can steal (start) another
   session's chained sentence.
3. A playlist interrupted by a session that then **fails its async load** is
   stranded in `Interrupted` forever: the interrupt stores the resume decision
   (`SessionManager.cpp:249`), but `consumeInterruptResumeDecision` is only
   reached from a runner's onFinish — and a session that never loaded never
   runs. `PlaylistEvent` deliberately stops rescheduling while `Interrupted`
   (`playlist.cpp:175-182`), so the chain is dead until a manual
   stop/start.

## Design

### Owner token

```cpp
struct StateOwner {
    std::string sessionId;          // exact-match cleanup
    uint64_t activityGeneration{0}; // total order over adoptions; stale guard
};
```

Both values already exist on `PlaybackSession` and are immutable after
adoption (generation is minted under the same `mutex_` that guards all of
this state), so no new synchronization and nothing new computed on the 1 ms
tick path.

### Chained animation queue → owner = stable chain id

The only producer is `StreamingAdHocSession` (sentence 2..n of an ad-hoc
speech chain). Per-hop playback-session ids won't work as owners — each
sentence gets a **new** session id when onFinish schedules it — so the owner
is the *chain id*: the `StreamingAdHocSession`'s own stable session id,
carried on every `PlaybackSession` in the chain.

- `PlaybackSession` gains an optional chain owner (id + the chain's first
  activity generation), set before publication like the other adoption fields.
- `queue<Animation>` becomes `deque<QueuedAnimation{Animation, StateOwner}>`.
- `queueAnimation(universe, animation, owner)` — producer passes its chain id.
- `popQueuedAnimation(universe, finisher)` pops the front entry **only if the
  finishing session carries the matching chain id** (one string compare under
  the already-taken mutex — tick-path safe). The scheduled next hop inherits
  the chain owner.
- `interrupt(...)` accepts the chain owner and stamps it on the session it
  creates, so sentence 1 is part of the chain too.
- `abortLoadingSession` replaces the queue `swap(empty)` with
  `std::erase_if(owner.sessionId == aborted session's chain id)` — a failing
  chain drops *its own* remaining sentences and nobody else's.

### Playlist state → explicit owner + interrupt owner

- `UniverseState` gains `StateOwner playlistOwner` (stamped by `startPlaylist`
  and updated by each `registerSession(isPlaylist=true)` adoption) and
  `StateOwner interruptOwner` (stamped by `interrupt()` alongside
  `shouldResumePlaylist`, cleared when the decision is consumed).
- `abortLoadingSession` clears playlist state **only** when the failing
  session is the exact current playlist owner (id match; generation must not
  be older than the stored owner's).

### Failed-interrupt resolution (the stranded-Interrupted fix)

`consumeInterruptResumeDecision`'s body moves to a private
`consumeInterruptResumeDecisionLocked()` (same semantics, same lock).
`abortLoadingSession` gains: if `playlistState == Interrupted` **and** the
failing session is the recorded `interruptOwner`, apply the stored decision
in-lock — resume → `Active` (+ status playing), decline → `Stopped`. The
result struct reports `playlistResumed` / `playlistStopped` so
`unwindAdoptedAudioSession` can schedule the next `PlaylistEvent` on resume
(it must start capturing `eventLoop`; today it doesn't) and broadcast the
*real* playlist status instead of the hardcoded empty one.

### Preserved invariants

- The atomic active-session ownership check (`SessionManager.cpp:710-716`)
  and the single-mutex critical section stay exactly as they are.
- No allocation, broadcast, or I/O added under `mutex_`; the only tick-path
  method that grows is `popQueuedAnimation` (one extra compare) — consistent
  with the issue #82 locking rules documented in `SessionManager.cpp`.
- `PlaylistEvent`'s Interrupted-case behavior is unchanged; the fix resolves
  the state at abort time rather than adding polling.

## Testing

SessionManager has **zero existing tests** and isn't linked into the test
target. Wiring it in:

- Add `SessionManager.cpp` + `PlaybackSession.cpp` to `creature-server-test`
  in `CMakeLists.txt`.
- Remove the three `SessionManager::` link-stubs from
  `tests/server/FakeIdleScheduling.cpp` (ODR collision otherwise); add small
  stubs for `PlaybackRunnerEvent`'s constructor and
  `CooperativeAnimationScheduler::scheduleAnimation`, which the real
  SessionManager references.

New `tests/server/animation/SessionManager_ownership_test.cpp`:

- Two non-conflicting sessions (disjoint creature sets) on one universe:
  both survive registration; generations ascend.
- A's chain entries survive `abortLoadingSession(B)`;
  `queuedAnimationsDropped == 0`; A's own abort drops exactly A's entries.
- Pop ownership: a finishing session without the chain id cannot pop another
  chain's entry; the owner can.
- Playlist owned by B survives a failing Playlist-reason session that is not
  the owner; the exact owner's failure clears it.
- Interrupted playlist + failing interrupter: resume decision `true` →
  `Active` with status playing (not `Interrupted`, not `None`); decision
  `false` → `Stopped`. Decision consumed exactly once.
- Stale abort (re-used session id from an older generation) cannot clear
  newer-generation state.

Plus the full existing suite.

## Files touched

| File | Change |
|---|---|
| `src/server/animation/SessionManager.h` | `StateOwner`, `QueuedAnimation`, owner fields, signatures |
| `src/server/animation/SessionManager.cpp` | owner stamping, owner-aware abort/pop/erase, locked decision helper |
| `src/server/animation/PlaybackSession.h/.cpp` | chain-owner field + accessors |
| `src/server/animation/CooperativeAnimationScheduler.cpp` | onFinish pop/inherit, unwind resume/stop handling, `eventLoop` capture |
| `src/server/voice/StreamingAdHocSession.cpp` | pass chain owner on interrupt + queueAnimation |
| `tests/server/animation/SessionManager_ownership_test.cpp` | new suite |
| `tests/server/FakeIdleScheduling.cpp` | remove colliding stubs, add needed ones |
| `CMakeLists.txt` | test-target wiring |
| `VERSION.txt` | 3.44.9 |
