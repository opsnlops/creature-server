# Issue #97 — RTP send-failure circuit breaker and worker-failure propagation

**Status:** Implemented (including code-review fixes below)
**Issue:** [#97](https://github.com/opsnlops/creature-server/issues/97)

## Post-review hardening (2026-08-21)

A high-effort adversarial review of the first implementation surfaced fixes
that are now part of the design:

- **Publish-before-release, everywhere.** `processOutputCommand` records the
  send outcome (and any terminal-record publication) *before* the
  `releaseAfterSend` release, and exceptions are handled inside
  `processOutputCommand` (`handleCommandException`) with the same ordering —
  the worker's outer catch is only a never-die guard. A reader that observes a
  released lease can therefore always find the trip's terminal record.
- **Readers re-check the registry after observing a released lease** (the
  publish-before-release ordering plus seq-cst atomics make that re-check
  sufficient); checking only before is a trip-lands-between-the-loads race.
- **Final-frame failure rule (April's call):** if the final frame set fails as
  part of a consecutive run ≥ 2, the generation is terminated
  (`final_frame_failure` reason) even though no threshold tripped — a released
  lease is only a delivery acknowledgment when the final send actually
  succeeded. An isolated single-frame blip on exactly the last frame still
  counts as delivered-with-loss, so one glitch cannot halt a show or playlist.
- **Reset exceptions** bypass the send-failure tracker (no send was attempted)
  but terminate the generation immediately (`reset_failed`): their lease is
  released regardless, and without a terminal record that release would read
  as a benign supersede.
- **Bounded cooperative drain:** after the final frame is enqueued the runner
  waits at most `MAX_FINAL_DRAIN_TICKS` (~2 s) for the worker's
  acknowledgment, then fails the session (`drain_timeout`) instead of ticking
  forever on a hung worker. (Standalone already had the 2 s completion-check
  bound.)
- **Standalone skipped-to-end path** no longer reports success at enqueue
  time; it routes through the completion check like normal completion.
- The completion check releases the lease on *every* failure path, the
  terminal-failure message is built in one place
  (`MultiOpusRtpServer::terminalFailureMessage`), and the worker's
  trace-context span attributes live in one helper.

## Problem

The RTP output worker (`src/server/rtp/MultiOpusRtpServer.cpp`) records one log +
one span + one counter per failed 17-channel frame set, unconditionally. A dead
socket therefore produces ~100 logs/spans per second forever, and — because
packet delivery is asynchronous — playback still reports success:

1. `RtpAudioTransport::dispatchNextChunk` (`src/server/audio/RtpAudioTransport.cpp:104-112`)
   treats a no-longer-current lease as benign ("stale", success). A breaker that
   releases the generation would be indistinguishable from a newer animation
   taking over.
2. `RtpAudioTransport::isFinished()` (`:200`) returns true once stopped, so the
   playback runner marks the session `completed_naturally` and `setSuccess()`
   (`src/server/eventloop/events/playback-runner.cpp:133-185`), broadcasting
   `ActivityState::Idle` to websocket clients.
3. The runner explicitly swallows audio dispatch errors (`playback-runner.cpp:124-130`,
   "Non-fatal - continue playback").
4. Standalone playback (`src/server/eventloop/events/music.cpp`) increments
   `soundsPlayed` and sets span success **at enqueue time** of the final frame
   (`music.cpp:109-118`), before the worker has sent anything, and treats a
   stale lease the same benign way (`music.cpp:58-64`).

## Design

### New header-only component: `src/server/rtp/RtpOutputHealth.h`

Two classes, no sockets, no globals, time injected — fully unit-testable
(`MultiOpusRtpServer` itself cannot be constructed in tests; it opens 17
multicast sockets).

**`RtpSendFailureTracker`** — owned by the worker thread, no locking needed.

- `Action recordFailure(uint64_t generation, steady_clock::time_point now)`
- `Action recordSuccess(uint64_t generation, steady_clock::time_point now)`
- `void beginGeneration(uint64_t generation)` — resets all state (called on
  `Reset` command; makes stale-generation state impossible to carry over)
- `Action` is `{emitDetail: bool, trip: bool, recovered: bool, suppressedSinceLastEmit: uint64_t}`

Tracking: consecutive-failure counter plus a coarse rolling window (fixed ring
of per-second buckets — no allocation, no `Date`-style wall-clock dependency).
Log/span gating follows the existing repo convention
(`AlsaAudioOutput.cpp:203-212`): emit on failure #1, every Nth thereafter, and
one recovery record; everything gated counts toward `suppressed`.

**`RtpTerminalFailureRegistry`** — the worker→owner channel that survives lease
release.

- Worker side: `publish(TerminalFailure)` where `TerminalFailure` carries
  generation, `rtp_error_t`, first failed channel, RTP timestamp, command type,
  frame index, and the full `AsyncAudioTraceContext` (owner/session/animation/
  sound-file/trigger trace+span IDs) — satisfying the issue's context-preservation
  criterion.
- Reader side (1 ms event loop): `bool isTripped(uint64_t generation)` — a
  single relaxed atomic load against a small fixed ring of tripped generations
  (fast path: not tripped ⇒ one atomic read, no lock). Detail fetch
  (`std::optional<TerminalFailure> find(generation)`) takes a mutex but is only
  reached after `isTripped` returns true — a rare, already-failed situation.
- Ring keeps the last 4 tripped generations so a terminal record remains
  readable after the lease is released and even after a new generation is
  acquired (the final-frame-failure case).

### Worker changes (`MultiOpusRtpServer.cpp`)

In `processOutputCommand`:

- `Reset` command → `tracker.beginGeneration(lease.generation)`.
- Send success (`result.error == RTP_OK`) → `recordSuccess`; if it reports
  recovery, emit one recovery log/span and increment the recovery counter.
- Send failure / exception → `recordFailure` via the existing funnels
  (`recordOutputFailure` / `recordOutputException`, both already `noexcept`
  with full command + trace context). Detail log/span only when
  `action.emitDetail`; add `rtp.failures.suppressed_since_last` to the span and
  bump the suppressed counter otherwise.
- On `action.trip`:
  1. `registry.publish(...)` **before** releasing anything (readers must never
     observe a released lease without a terminal record).
  2. Release using the worker's existing idiom (`MultiOpusRtpServer.cpp:257-262`):
     `outputCoordinator_.release(lease)`, `rtcpSender_.endSession(gen)`,
     `readyGeneration_` CAS to 0 — outside the coordinator `Guard` (its mutex is
     non-recursive).
  3. Drop remaining queued commands for that generation
     (`outputQueue_.eraseIf`, existing pattern at `:140-141`).
  4. Emit one `rtp.output.circuit_open` span (error) + increment circuit-open
     and terminal-generation counters.
  5. Broadcast a client notice via `creatures::broadcastNoticeToAllClients()` —
     the same mechanism the shutdown path uses (`main.cpp:525`) — so connected
     consoles see "RTP audio output failed" immediately. The broadcast enqueues
     onto the thread-safe websocket outgoing queue, so it is safe from the
     worker thread. (April requested this during review, 2026-08-21.)

Nothing here blocks, and nothing runs on the 1 ms event loop.

New public API on `MultiOpusRtpServer`:

- `bool isGenerationTripped(uint64_t generation) const noexcept` — atomic fast path
- `std::optional<rtp::TerminalFailure> terminalFailureFor(uint64_t generation) const`

### Cooperative propagation

**`RtpAudioTransport`** (`src/server/audio/RtpAudioTransport.cpp`):

- In `dispatchNextChunk`, *before* the stale-lease check at `:104`: if
  `isGenerationTripped(lease.generation)` → set a new `terminalFailure_` flag,
  stop the transport, mark the work span `rtp.work.outcome = "circuit_open"` +
  error, and return an **error** `Result`.
- New accessor `hasTerminalFailure()`. `isFinished()` still returns true when
  stopped (the runner must not spin forever), but the runner checks failure
  before treating "finished" as success.
- The final-frame path: after `finalFrameQueued_`, the runner keeps ticking
  until `areAllTracksFinished()`; the transport's finished/failure check
  consults the registry so a failed *final* frame set (worker already released
  the lease) is still seen as terminal failure, not natural completion.

**`PlaybackRunnerEvent`** (`playback-runner.cpp`):

- Replace the swallow at `:124-130` for the terminal case only: if the dispatch
  result is an error **and** the transport reports terminal failure, mirror the
  DMX-failure unwind at `:109-121` — `session_->cancel()`,
  `completeCancelledSession()` — broadcasting `ActivityReason` /
  `ActivityState::Stopped` instead of `Idle`, and marking the runner span as
  error. Transient dispatch errors keep today's warn-and-continue behavior.
- Before `markFinished()` at `:133-134`: if any track reports terminal failure,
  take the failure unwind instead of the success path.

`PlaybackSession::markFinished()` is one-shot, so the completion/failure race
stays idempotent. The `onFinish`/queue/playlist behavior after a cancel is the
already-shipped #79 path — no new states invented (a proper
`ActivityState::Failed` is out of scope; noted as follow-up).

### Standalone propagation

**`StandaloneRtpFrameEvent`** (`music.cpp`):

- Top of `executeImpl`, before the stale branch at `:58`: registry check →
  route into the existing `fail()` (`:128-137`), which already releases the
  lease and sets the span error. The admission `Reservation` is freed by
  `shared_ptr` destruction either way.
- Final frame: move success reporting (`soundsPlayed` increment + span success,
  `:109-118`) out of the enqueue branch into a small follow-up
  `StandaloneRtpCompletionCheckEvent` scheduled a few frames after the final
  enqueue. It queries the registry: tripped → `fail()`-equivalent reporting;
  clean → the success reporting exactly as today. One extra event per standalone
  playback, negligible.

### Metrics

Three new counters through the standard five-touch pattern
(`counters.h` / `counters.cpp` / `ObservabilityManager.{h,cpp}` delta export;
`CounterSendEvent` picks them up for websocket + Honeycomb automatically):

- `creature_server_rtp_circuit_breaker_trips`
- `creature_server_rtp_send_failures_suppressed`
- `creature_server_rtp_send_recoveries`

The issue's "terminal-generation" counter is deliberately omitted: with one
active output generation at a time, a trip *is* a terminal generation — the
two counters could never diverge, and a counter that can't diverge from
another is dashboard confusion, not signal.

### Thresholds (defaults live in `RtpBreakerConfig`, `src/server/rtp/RtpOutputHealth.h`,
colocated with the logic that uses them)

- **Trip:** 50 consecutive failed frame sets (~0.5 s at the 100/s frame rate) —
  fast enough that a dead socket doesn't burn spans for long, slow enough that
  a transient blip (a few frames) never trips.
- **Windowed trip:** ≥ 300 failures within the last 10 one-second buckets
  (catches persistent-but-intermittent flapping that resets the consecutive
  counter).
- **Log/span gate:** detail on failure #1, then every 100th; recovery always.

### Tests (`tests/server/rtp/`, registered in `CMakeLists.txt`)

Deterministic, injected-time unit tests against `RtpOutputHealth.h`:

- intermittent failure: successes interleaved → never trips, recovery records emitted
- persistent failure: trips at exactly the consecutive threshold; suppression
  counts correct between emissions
- windowed flapping: trips via the window despite consecutive resets
- recovery: failure run → success → recovery action exactly once
- stale generation: `beginGeneration` wipes prior state; failures from a prior
  generation can't contribute to the new one's trip
- registry: publish → readable after simulated release; ring eviction keeps the
  last 4; `isTripped` false for untripped/unknown generations
- final-frame failure: record published for generation G remains readable after
  a new generation G+1 is acquired

Plus the existing full suite (`creature-server-test`) must stay green.

## Explicit non-goals

- No new `ActivityState::Failed` websocket state (follow-up candidate; today's
  vocabulary is Running/Idle/Disabled/Stopped and clients depend on it).
- No retry/reconnect of the underlying uvgRTP streams — the breaker stops the
  doomed generation; the next playback acquires a fresh generation and probes
  the socket naturally.
- No changes to issue #92's broader observability checklist beyond what the
  breaker itself requires.

## Files touched

| File | Change |
|---|---|
| `src/server/rtp/RtpOutputHealth.h` | new — tracker + registry (header-only) |
| `src/server/rtp/MultiOpusRtpServer.{h,cpp}` | breaker wiring, trip path, query API |
| `src/server/audio/RtpAudioTransport.{h,cpp}` | terminal-failure detection + error result |
| `src/server/eventloop/events/playback-runner.cpp` | fatal audio branch mirroring DMX unwind |
| `src/server/eventloop/events/music.cpp` | registry checks + deferred completion check |
| `src/server/metrics/counters.{h,cpp}` | four counters |
| `src/util/ObservabilityManager.{h,cpp}` | instruments + delta export |
| `src/server/config.h` | threshold constants |
| `tests/server/rtp/RtpOutputHealth_test.cpp` | new test suite |
| `CMakeLists.txt` | register test file |
