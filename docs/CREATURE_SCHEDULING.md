# Creature Scheduling Design (draft)

Goals:
- Per-creature orchestration with “last request wins” per creature (and BGM).
- Multi-creature animations coordinate only the involved creatures; others stay untouched.
- Preserve 1 ms event loop; keep fixed RTP channel mapping from Creature config.

Key invariants:
- Single DMX universe in memory; event loop stays central.
- Creature config remains source of truth (channel offsets, audio_channel, mouth_slot, idle lists).
- Universe mapping stays runtime-only.

States (per creature scheduler):
- IdleEnabled: boolean flag (API-togglable; defaults enabled).
- Session:
  - None
  - ActiveSingle(creature_session_id)
  - ActiveJoint(joint_session_id)  // shared across multiple creatures
- IdleRun:
  - None
  - Running(id: animation_id)       // selected from creature idle list
- Pending: none (new request immediately cancels current session: last request wins).

State transitions (per creature):
- StartSingle(animation A for creature C):
  - Cancel any current session (single or joint) affecting C; stop idle if running.
  - Create session (single); link to scheduler; start runner.
- StartJoint(animation A with creatures {C1..Cn}):
  - For each Ci in available set, cancel their current session; stop idle.
  - Create one joint session (shared runner, shared clock); link to each Ci scheduler as ActiveJoint.
  - Missing/unavailable creatures are skipped; record skipped list on the session.
- SessionComplete (natural):
  - Clear session reference on involved creature(s).
  - If IdleEnabled and idle list non-empty: start IdleRun (pick random from list; no quiet delay).
  - If session was joint, completion applies to all linked creatures.
- SessionCancelled (preempt or explicit):
  - Teardown (status light off, audio stop) for involved creature(s) only.
  - Clear session ref; if IdleEnabled start idle.
- IdleStart:
  - Preconditions: IdleEnabled && no active session.
  - Choose random idle animation from creature config list; start as single session flagged “idle”.
- IdleStop (API toggle or new session preempt):
  - Cancel idle session (if any); set IdleEnabled=false when toggled off.
- IdleToggle API:
  - enable/disable per creature; broadcast WS message with new state to all clients.

Audio model:
- Per-creature RTP channel is fixed from Creature.audio_channel; clients already listen per-creature + BGM.
- Sessions reset SSRC only for channels they own (per-session); multiple sessions can stream concurrently on distinct channels.
- BGM is global last-writer-wins; ad-hoc animations do not touch BGM.
- SDL path unchanged; scoped to the session.

DMX/runner model:
- One runner per active session (single or joint). Joint session runner owns tracks for all involved creatures and uses shared clock.
- Runner emits DMX only for involved creatures’ tracks; others unaffected.
- Event queue remains shallow: runner + current-frame DMX/audio events.

Partial availability:
- Multi-creature animation with missing creatures: run subset; record skipped creature_ids for response/telemetry.
- If a creature lacks universe/channel mapping at runtime, skip and log; others proceed.

API/DTO impacts (to design in detail next):
- Creature GET DTO: include runtime sub-object (empty in config if absent):
  - idle_animation_ids: [string] // optional; idle loop candidates stored with creature config
  - runtime: {
      idle_enabled: bool,
      activity: {state: running|idle|disabled|stopped, animation_id: string|null, session_id: uuid|null, started_at: iso8601, updated_at: iso8601, reason: play|ad_hoc|playlist|idle|disabled|streaming},
      counters: {sessions_started_total: uint64, sessions_cancelled_total: uint64, idle_started_total: uint64, idle_stopped_total: uint64, idle_toggles_total: uint64, skips_missing_creature_total: uint64, bgm_takeovers_total: uint64, audio_resets_total: uint64},
      bgm_owner: creature_id|null,
      last_error: {message: string, timestamp: iso8601}|null
    }
- Idle toggle endpoints per creature (enable/disable).
- New WS messages:
  - idle_state_changed {type, creature_id, idle_enabled, timestamp: iso8601}
  - creature_activity {type, creature_id, state: running|idle|disabled|stopped, animation_id: string|null, session_id: uuid|null, reason: play|ad_hoc|playlist|idle|disabled, timestamp: iso8601}

Observability/metrics:
- Spans tagged with creature_id(s), session_id (stable UUID per playback), scheduler_type (single|joint), reason (idle|play|ad_hoc|playlist).
- REST success responses that schedule playback include session_id for debugging (ad-hoc, prepared play, and cooperative play/interrupt/playlist where a session exists).
- Per-creature counters: sessions_started, sessions_cancelled, idle_cycles, idle_toggles, audio_resets, skips_missing_creature.
- Gauges: per-creature active_session (bool), bgm_owner (creature_id or none).

Current implementation snapshot:
- Runtime DTO is live: includes idle_enabled, activity (state/reason/animation_id/session_id/timestamps), counters, bgm_owner, last_error.
- Activity enums enforced server-side: state = running|idle|disabled|stopped; reason = play|playlist|ad_hoc|idle|disabled|cancelled|streaming.
- Streaming mode:
  - Live streaming cancels overlapping sessions and marks activity reason=streaming (running).
  - Streaming stop is auto-emitted after a short timeout when frames cease (default 60 frames / ~60ms, configurable via
    `--streaming-timeout-frames` or `STREAMING_TIMEOUT_FRAMES`).
  - Animation play requests fail with 409 Conflict if any involved creature is streaming.
- WebSocket messages implemented:
  - idle-state-changed {creature_id, idle_enabled, timestamp}
  - creature-activity {creature_id, state, animation_id, session_id, reason, timestamp}
- Session IDs are stable per playback and reused on completion/cancel; returned by REST for ad-hoc/interrupt and cooperative play/playlist when a session exists.
- Activity transitions: completion -> idle (or disabled if idle is off); cancel -> stopped with reason cancelled; idle request remaps to disabled when idle is off.
- BGM is last-request-wins; ad-hoc animations skip BGM.

Client alignment (creature-console):
- Common package consumes idle-state/creature-activity via enums; CLI prints them; GUI logs and posts notifications. StatusDTO includes optional session_id.

CI/testing note:
- Skip vendor mongo driver fixtures/import tests with regex:
  mongoc/fixtures/fake_kms_provider_server/start|
  mongoc/CMake/bare-bson-import|
  mongoc/CMake/bare-mongoc-import|
  mongoc/CMake/bson-import-1.0|
  mongoc/CMake/bson-import-range-upper|
  mongoc/CMake/bson-import-range-lower|
  mongoc/CMake/bson-import-major-range|
  mongoc/CMake/bson-import-opt-components
  (Used in GitHub Actions `tests.yml` ctest invocation.)

Open TBDs (capture later):
- Exact counter names and DTO fields.
- Session ID format and exposure in APIs.

API sketch (proposal):
- GET /api/v1/creature/{id}
  - Body includes config plus runtime object (see above).
- PATCH /api/v1/creature/{id}/idle
  - Body: {enabled: bool, reason?: string}
  - Response: updated creature with runtime snapshot
  - Side effects: cancel idle if disabling; broadcast idle_state_changed
- (Optional) GET /api/v1/creature/{id}/activity
  - Returns runtime.activity and counters (same shape as runtime section above)

WebSocket payloads (proposal):
- idle_state_changed:
  - {type: "idle_state_changed", creature_id, idle_enabled, timestamp: iso8601}
- creature_activity:
  - {type: "creature_activity", creature_id, state: "running"|"idle"|"disabled"|"stopped", animation_id: string|null, session_id: uuid|null, reason: "play"|"ad_hoc"|"playlist"|"idle"|"disabled", timestamp: iso8601}

Counters and telemetry (proposal):
- Counters per creature (monotonic):
  - sessions_started_total
  - sessions_cancelled_total
  - idle_started_total
  - idle_stopped_total
  - idle_toggles_total
  - skips_missing_creature_total
  - bgm_takeovers_total
  - audio_resets_total
- Gauges:
  - active_session (0/1)
  - bgm_owner (creature_id or null)
- Spans:
  - Tags: creature_id(s), session_id (uuidv4), scheduler_type (single|joint), reason (play|ad_hoc|playlist|idle|disabled), skipped_creatures[]
- Session IDs: UUIDv4 per session (single or joint) and exposed in activity/WS.
