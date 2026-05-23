# DmxFixture: First-Class Light & DMX Device Support

## Context

The creature-server is built around DMX for animatronic control, but DMX is fundamentally a lighting protocol. April wants first-class support for non-Creature DMX devices — starting with lights, but generic enough to handle smoke machines, foggers, etc. without bolting them onto the `Creature` model.

The new concept is **`DmxFixture`**: a device with named **channels** (red, green, blue, brightness, blink, fog_output, etc.) instead of servo axes. Fixtures must support two control paths:

1. **Animations** — fixtures can be tracks in an existing `Animation`, just like creatures.
2. **Patterns + bindings** — declarative trigger like *"hold a red glow on this light while creature Beaky is speaking"*, without authoring a full animation.

The design must keep `Creature` untouched and feel natural in the existing architecture. Fixtures intentionally diverge from creatures on two points: the server's DB is the source of truth (most fixtures have no controller), and universe assignments are persisted (not lost on restart).

## Design Decisions (confirmed with April)

- **Naming**: `DmxFixture` (generic; UI specializes by `type` field — `"light"`, `"smoke_machine"`, `"fogger"`, `"generic"`).
- **Animation integration**: Extend the existing `Track` with a `fixture_id` field alongside `creature_id`. Exactly one must be set per track.
- **Triggers**: Patterns w/ fade-in / fade-out / hold, attached to fixtures via bindings.
- **Bindings live on the fixture config** — fixtures are self-describing; no separate Binding collection.
- **Source of truth**: the server's MongoDB is authoritative for fixtures. Fixtures are managed entirely from the Creature Console Swift app via the REST API — there is no on-disk fixture config file and no controller registration step. *This is an intentional divergence from the creature model — call it out in AGENTS.md.*
- **Universe assignment is persisted** on the fixture document as an `assigned_universe` field (nullable). Lost-on-restart semantics would force the user to re-assign every fixture by hand; that's poor UX for stage lights wired to fixed DMX addresses. Runtime lookups still go through `fixtureUniverseMap`, which is hydrated from the DB at server startup.

## Data Model

### Fixture JSON (API request body — submitted by the Creature Console Swift app)

Fixtures are created and edited in the Creature Console UI. The Swift app PUTs/POSTs this JSON to the server. The server stores it in MongoDB; the DB is the source of truth. There is **no on-disk fixture config file** and no controller pushing this.

Realistic example. All IDs in this project are **UUIDs** (RFC 4122, 8-4-4-4-12 hex form). `creature_id` references the same ID type as `Creature.id` — *not* a human-readable name like `"beaky"`:

```json
{
  "id": "8e3a4b5c-1d2f-4e6a-9b0c-7f8e9d0a1b2c",
  "name": "Stage Left Spot",
  "type": "light",
  "channel_offset": 500,
  "assigned_universe": 1,
  "channels": [
    { "offset": 0, "name": "red",        "kind": "color_red" },
    { "offset": 1, "name": "green",      "kind": "color_green" },
    { "offset": 2, "name": "blue",       "kind": "color_blue" },
    { "offset": 3, "name": "white",      "kind": "color_white" },
    { "offset": 4, "name": "blink",      "kind": "generic" },
    { "offset": 5, "name": "brightness", "kind": "master_dimmer" }
  ],
  "patterns": [
    {
      "id": "7d2a3b4c-5e6f-4789-a0b1-c2d3e4f5a6b7",
      "name": "Red Glow",
      "values": [
        { "channel": "red",        "value": 255 },
        { "channel": "brightness", "value": 200 }
      ],
      "fade_in_ms": 250,
      "fade_out_ms": 500,
      "hold_ms": 0
    }
  ],
  "bindings": [
    {
      "creature_id": "1a2b3c4d-5e6f-4789-a0b1-c2d3e4f5a6b7",
      "on_reason":   "ad_hoc",
      "on_state":    "running",
      "pattern_id":  "7d2a3b4c-5e6f-4789-a0b1-c2d3e4f5a6b7"
    }
  ]
}
```

### JSON Schema Reference

This section is the authoritative API contract. The Creature Console Swift app implements its data model from this; the server validates against it. Field names use `snake_case` over the wire (matching existing creature DTOs).

**Object: `DmxFixture` (root)**

| Field | Type | Required | Default | Notes |
|---|---|---|---|---|
| `id` | string (UUID) | yes | — | Fixture identifier. RFC 4122 UUID (8-4-4-4-12 lowercase hex). Same convention as `Creature.id` — this project uses UUIDs throughout, never MongoDB OIDs. Primary key in MongoDB. Generated client-side by the Swift app when creating a new fixture. |
| `name` | string | yes | — | Human-readable display name. Non-empty. |
| `type` | enum string | yes | — | One of `"light"`, `"smoke_machine"`, `"fogger"`, `"generic"`. Unknown strings parse to `"generic"` with a server warning (be liberal). UI uses this to pick a renderer. |
| `channel_offset` | uint16 | yes | — | Starting DMX channel within the universe. Range `[0, 511]`. The fixture occupies channels `channel_offset` through `channel_offset + max(channels[].offset)` inclusive. |
| `assigned_universe` | uint32 \| null | no | `null` | Persisted E1.31 universe assignment. Valid range `[1, 63999]` per E1.31 spec — 0 and >63999 are rejected with 400. `null` means the fixture is configured but produces no DMX output. Settable via `PUT /api/v1/fixture/{id}/universe`. |
| `channels` | array of `FixtureChannel` | yes | — | Non-empty. **Max 64 entries** (anti-DoS cap). Defines the addressable channels of this fixture. |
| `patterns` | array of `FixturePattern` | no | `[]` | **Max 256 entries.** Named DMX value snapshots that bindings can trigger. |
| `bindings` | array of `FixtureBinding` | no | `[]` | **Max 256 entries.** Declarative triggers connecting creature activity transitions to patterns on this fixture. |

**Object: `FixtureChannel`**

| Field | Type | Required | Default | Notes |
|---|---|---|---|---|
| `offset` | uint16 | yes | — | Channel offset relative to `DmxFixture.channel_offset`. Channel's absolute DMX address = `channel_offset + offset`. `[0, 511 - channel_offset]`. |
| `name` | string | yes | — | Channel name (e.g. `"red"`, `"brightness"`, `"fog_output"`). Unique within the fixture. Used by `FixturePatternValue.channel` to reference this slot. |
| `kind` | string | no | `"generic"` | UI hint only — server never branches on it. Conventional values: `"color_red"`, `"color_green"`, `"color_blue"`, `"color_white"`, `"color_amber"`, `"color_uv"`, `"master_dimmer"`, `"strobe"`, `"pan"`, `"tilt"`, `"gobo"`, `"generic"`. New `kind` strings can be added at any time without server changes. |

**Object: `FixturePattern`**

| Field | Type | Required | Default | Notes |
|---|---|---|---|---|
| `id` | string (UUID) | yes | — | Pattern identifier (UUID). Unique within the fixture. Referenced by `FixtureBinding.pattern_id` and by the manual-trigger endpoint. Generated client-side. |
| `name` | string | yes | — | Display name for the UI. |
| `values` | array of `FixturePatternValue` | yes | — | **Max 64 entries.** Target channel values. Channels not listed are not driven by this pattern (preserve whatever the previous render had). |
| `fade_in_ms` | uint32 | no | `0` | Milliseconds to ramp from the channels' current values to the pattern's target values. `0` = snap. |
| `fade_out_ms` | uint32 | no | `0` | Milliseconds to ramp back to pre-pattern values when the pattern stops. `0` = snap. |
| `hold_ms` | uint32 | no | `0` | Milliseconds to hold the target values after fade-in. `0` = hold indefinitely until an external stop (e.g. the originating binding's state transitions out). |

**Object: `FixturePatternValue`**

| Field | Type | Required | Default | Notes |
|---|---|---|---|---|
| `channel` | string | yes | — | Must match a `FixtureChannel.name` on the parent fixture. Validated server-side. |
| `value` | uint8 | yes | — | DMX value, range `[0, 255]`. |

**Object: `FixtureBinding`**

| Field | Type | Required | Default | Notes |
|---|---|---|---|---|
| `creature_id` | string UUID (`creatureId_t`) | yes | — | UUID of the creature whose activity transitions this binding observes. Same ID type as `Creature.id` — **not a human-readable name**. The server does not validate that the referenced creature currently exists (creatures can be added/removed independently); the binding is dormant if the creature is absent. |
| `on_reason` | enum string \| null | no | `null` | Filter on activity reason. One of `"play"`, `"playlist"`, `"ad_hoc"`, `"idle"`, `"disabled"`, `"cancelled"`, `"streaming"`, or `null` for wildcard. Matches `runtime::ActivityReason` (`src/server/runtime/Activity.h`). |
| `on_state` | enum string \| null | no | `null` | Filter on activity state. One of `"running"`, `"idle"`, `"disabled"`, `"stopped"`, or `null` for wildcard. Matches `runtime::ActivityState`. |
| `pattern_id` | string (UUID) | yes | — | Must reference an `id` of a `FixturePattern` defined on this same fixture. Validated server-side. |

**Cross-field validation rules** (server enforces, returns 400 on failure):

1. `channel_offset + max(channels[].offset)` ≤ 511 (fixture must fit within universe).
2. `channels[].name` is unique within `channels[]`.
3. `patterns[].id` is unique within `patterns[]`.
4. Each `patterns[].values[].channel` references an existing `channels[].name` on this fixture.
5. Each `bindings[].pattern_id` references an existing `patterns[].id` on this fixture.
6. `bindings[].on_reason` / `on_state`, when present, parse to known enum values.
7. `bindings[]` does not validate `creature_id` existence (soft reference — creatures can be deleted independently).
8. `assigned_universe`, when present, must be in `[1, 63999]` (E1.31).
9. Array size caps: `channels[]` ≤ 64, `patterns[]` ≤ 256, `patterns[].values[]` ≤ 64, `bindings[]` ≤ 256.

The validate-only endpoint (`POST /api/v1/fixture/validate`) returns these errors structured for UI display rather than a single 400 string. The response also includes `missing_creature_ids[]` — `creature_id`s referenced by bindings that don't exist in the creature collection. Missing creatures are a soft warning, not a failure; the fixture still saves.

**Path-parameter validation** (controllers, returns 400 on failure):

- Any `{fixtureId}` or `{patternId}` in a URL must match RFC 4122 UUID shape (8-4-4-4-12 lowercase hex). Anti-injection guard — keeps malformed strings out of log lines and span attributes. Hit by every endpoint that takes a path param.

**Trigger endpoint constraints** (`POST /api/v1/fixture/{id}/pattern/{pid}/trigger`):

- Body is optional. With no body the pattern runs with its configured fade-in / hold / fade-out and stays held until something else stops it.
- `stop_after_ms` (UInt32, optional): if set, must be in `(0, 600000]` ms (10 minute cap). `0` rejected; `> 600000` rejected. Server schedules an auto-stop event at `trigger_frame + stop_after_ms`.
- The fixture must have an `assigned_universe` (set via `PUT /api/v1/fixture/{id}/universe`) or the trigger returns 400. There is no "default universe" fallback.

### C++ types

Add `src/model/DmxFixture.h/.cpp` mirroring `Creature.h/.cpp`. Inline the small leaf types (`FixtureChannel`, `FixturePattern`, `FixturePatternValue`, `FixtureBinding`) inside the same header — they only matter to the fixture. Standard `convertToDto` / `convertFromDto` free functions, with `DmxFixtureDto` / `FixtureChannelDto` / `FixturePatternDto` / `FixtureBindingDto`.

`FixtureType` enum: `Light | SmokeMachine | Fogger | Generic`. Unknown `type` strings parse to `Generic` with a warn (be liberal — vendors invent device types faster than we can enumerate them).

## New Files

| File | Role | Mirrors |
|---|---|---|
| `src/model/DmxFixture.h` / `.cpp` | Struct + DTOs + converters | `src/model/Creature.h` / `.cpp` |
| `src/server/fixture/helpers.cpp` | `fixtureFromJson`, JSON validation | `src/server/creature/helpers.cpp` |
| `src/server/fixture/upsert.cpp` | JSON → BSON → MongoDB + cache | `src/server/creature/upsert.cpp` |
| `src/server/fixture/get.cpp`, `getall.cpp` | Read paths | `src/server/creature/get*.cpp` |
| `src/server/ws/controller/DmxFixtureController.h` | REST endpoints | `src/server/ws/controller/CreatureController.h` |
| `src/server/ws/service/DmxFixtureService.{h,cpp}` | Business logic (CRUD + universe assignment) | `src/server/ws/service/CreatureService.{h,cpp}` |
| `src/server/ws/dto/SetFixtureUniverseRequestDto.h` | `{universe: UInt32}` body for the universe endpoint | (new) |
| `src/server/ws/dto/TriggerFixturePatternRequestDto.h` | `{stop_after_ms?: UInt32}` body for the manual trigger endpoint | (new) |
| `src/server/ws/dto/SetFixtureLiveRequestDto.h` | `{values: [{channel, value}], timeout_ms}` body for live control | (new) |
| `src/server/ws/dto/FixtureConfigValidationDto.h` | Validation response | `CreatureConfigValidationDto.h` |
| `src/server/fixture/FixturePatternRunner.{h,cpp}` | Active-pattern map + lerp engine + live-control map | (new) |
| `src/server/fixture/FixturePatternTickEvent.{h,cpp}` | Eventloop tick (~50 Hz) for fade interpolation + live emit | (new) |
| `src/server/fixture/FixtureBindingDispatcher.{h,cpp}` | Matches activity transitions → pattern starts | (new) |

## Existing Files to Modify

- **`src/model/Track.h` / `.cpp`** — Add `std::string fixture_id;` (empty = unset). Exactly one of `creature_id` / `fixture_id` is set per track; validate this in `animation/helpers.cpp`. BSON-schemaless + oatpp `info->required = false` makes existing DB documents backwards-compatible.
- **`src/server/animation/PlaybackSession.{h,cpp}`** — `TrackState` gets a sibling `std::string fixtureId;` populated from the source Track.
- **`src/server/eventloop/events/playback-runner.cpp` `emitDmxFrames()` (≈ lines 257–341)** — Branch: if `trackState.fixtureId` is set, look up `fixtureCache->get(fixtureId)`, use `fixture->channel_offset`, and resolve universe via `fixtureUniverseMap->get(fixtureId)` (fixtures live in their own universe, independent of the animation's session universe).
- **`src/server/ws/service/CreatureService.cpp` `setActivityState()`** — After `broadcastCreatureActivity(...)`, call `FixtureBindingDispatcher::onCreatureActivity(creatureId, reason, state, span)`. Dispatcher is edge-triggered: keeps a `creatureId → (lastReason, lastState)` map to suppress duplicate transitions.
- **`src/server/main.cpp`** — Add globals `fixtureCache`, `fixtureUniverseMap`, `fixturePatternRunner`, `fixtureBindingDispatcher` in the existing extern-cache namespace block. On startup, after the DB connection is up, hydrate `fixtureCache` from `getAllFixtures()` and populate `fixtureUniverseMap` from each fixture's `assigned_universe`. Register `DmxFixtureController` wherever `CreatureController` is wired.
- **`src/server/config.h`** — `#define FIXTURES_COLLECTION "fixtures"` next to the existing collection constants.
- **`src/model/CacheInvalidation.h`** — Add `CacheType::Fixture` so the cache invalidation events used after `upsertCreature` work for fixtures too.
- **`AGENTS.md`** — New section "DMX Fixture Model" after the Creature section.

## REST Endpoints

This is the contract the Creature Console Swift app codes against.

**CRUD** (similar to creatures, but no `register` endpoint — fixtures aren't pushed by controllers):

- `GET    /api/v1/fixture` — list all
- `GET    /api/v1/fixture/{fixtureId}` — get one
- `POST   /api/v1/fixture` — create or update (BODY_STRING raw JSON, schema above)
- `DELETE /api/v1/fixture/{fixtureId}` — remove a fixture
- `POST   /api/v1/fixture/validate` — validate JSON without saving (useful for the Swift app's form validation)

**Universe assignment** — fixtures need a DMX universe to output to. Persisted on the fixture document as `assigned_universe`; `fixtureUniverseMap` is hydrated from the DB at startup for fast runtime lookups.

- `PUT    /api/v1/fixture/{fixtureId}/universe` — body `{universe: UInt32}`. Persists to DB + updates `fixtureUniverseMap`. 404 if the fixture doesn't exist.
- `DELETE /api/v1/fixture/{fixtureId}/universe` — clear the assignment (fixture goes dark until reassigned). The current universe is also visible on the fixture document via `GET /api/v1/fixture/{fixtureId}`, so no dedicated GET endpoint is needed.

**Manual pattern trigger** — for testing and ad-hoc UI control:
- `POST   /api/v1/fixture/{fixtureId}/pattern/{patternId}/trigger` — manually fire a saved pattern (skips binding match). Body optional `{stop_after_ms?: number}`.

**Preview an unsaved pattern** — fire what the editor has on screen without an upsert round-trip:
- `POST   /api/v1/fixture/{fixtureId}/pattern/preview` — body `{values: [{channel, value}], fade_in_ms?, fade_out_ms?, hold_ms?, stop_after_ms?}`. The pattern is constructed ephemerally from the body and handed to the same runner path as the regular trigger; nothing is persisted. Same validation as a saved pattern (every channel must exist on the fixture; assigned_universe required); live control still preempts.

**Live control (slider UI)** — drive raw DMX values directly with auto-blackout:
- `POST   /api/v1/fixture/{fixtureId}/live` — body `{values: [{channel: string, value: 0..255}], timeout_ms: UInt32}`. `timeout_ms` is **required**, must be in `(0, 600000]` (10 min cap). The server holds the values until the deadline elapses, then blacks out all channels on this fixture. Channels not named in a call hold their previous live value within the same session (or 0 on the first call). Sending another live call before the deadline extends/replaces the deadline.
  - **Mutual exclusion with patterns.** Live arriving cancels any active pattern hard (no fade-out). While live is in effect, new patterns are *refused* (`/trigger` and binding-driven starts return false / 400). Once live expires (or the slider window stops sending), normal patterns work again.
  - **400 cases**: empty `values`, missing/zero/over-cap `timeout_ms`, unknown channel name (whole call fails — no partial application), fixture has no assigned universe.

## Pattern Execution Engine (the new piece)

### Architecture

A single global `FixturePatternRunner` owns an `unordered_map<fixtureId, ActivePattern>`. A self-scheduling `FixturePatternTickEvent` reschedules itself every **20 frames (20 ms ≈ 50 Hz)** while any patterns are active, then naturally pauses when the map is empty. The dispatcher re-arms it on the next trigger. Patterns and animations share the same single-threaded eventloop — no extra threads, no extra locks.

20 ms is well-matched to DMX refresh (~44 Hz) and to what stage lighting consoles use. Running pattern interpolation at the 1 ms event loop tick rate would waste CPU on values no receiver can show.

### `ActivePattern` state per fixture
```cpp
struct ActivePattern {
    std::string fixtureId;
    std::string patternId;
    universe_t  universe;
    framenum_t  startedAtFrame;     // 1ms-resolution frame numbers (existing convention)
    framenum_t  fadeInDoneFrame;
    framenum_t  holdDoneFrame;      // FRAMENUM_MAX if hold_ms==0 ("hold until stopped")
    framenum_t  fadeOutDoneFrame;
    Phase phase;                    // FadeIn | Hold | FadeOut | Done
    std::vector<uint8_t> startValues;   // current DMX bytes at trigger time (snapshot)
    std::vector<uint8_t> targetValues;  // computed from pattern.values
    uint16_t channelOffset;
    uint16_t channelSpan;
    std::string creatureId;             // which trigger started this
};
```

### Trigger flow

1. `CreatureService::setActivityState` calls `FixtureBindingDispatcher::onCreatureActivity(creatureId, reason, state, span)`.
2. Dispatcher iterates `fixtureCache->getAllKeys()` → each fixture → its `bindings[]`.
3. A binding matches when `creature_id == creatureId` AND (`on_reason` is wildcard OR matches) AND (`on_state` is wildcard OR matches).
4. On match: look up pattern by id, look up fixture's universe via `fixtureUniverseMap`. If the fixture has no `assigned_universe` (user hasn't set one yet), log debug and bail — no DMX output for that fixture.
5. `runner->start(fixture, pattern, universe, creatureId, currentFrame)` snapshots current `targetValues` (from the previous ActivePattern, if any) as the new `startValues`, computes new `targetValues` by writing each `pattern.values[i].value` at offset `channel_offset + channel.offset`, and replaces the entry in the map.
6. If the tick event isn't already armed, arm it.

### Tick processing (each ~20 ms)

For each `ActivePattern`:
- `elapsedMs = currentFrame - startedAtFrame`.
- **FadeIn** (`elapsedMs < fade_in_ms`): lerp `startValues → targetValues` by `elapsedMs / fade_in_ms`.
- **Hold** (after fade-in; before `holdDoneFrame`): output `targetValues`.
- **FadeOut**: lerp back to `startValues`. After fade-out → `Done`, removed from map.
- Build one `DMXEvent { universe, channelOffset, data=channelSpan bytes }` per active fixture and schedule it on the eventloop.

### Stopping

A pattern stops when the same `(creatureId, fixture)` binding match resolves to a terminal state (e.g. `Stopped` or back to `Idle` from a non-idle reason). The dispatcher calls `runner->stop(fixtureId, currentFrame)` which advances the entry into FadeOut phase.

### Concurrency rule: two creatures, one fixture

**Last-wins, smooth handoff.** A new `start()` on a fixture that already has an `ActivePattern` uses the current rendered DMX bytes as the new `startValues`. Old pattern is replaced — no queue, no priority. Guarantees no DMX snap.

### Precedence: animation tracks vs patterns

**Animation tracks win.** If an animation is currently playing a track targeting `fixture_id`, the runner pauses its output for that fixture (queries `sessionManager` by fixture id). Track completion re-enables the pattern next tick. This avoids the two systems fighting on a per-byte basis.

## Live Control Engine

A second control path alongside patterns, for slider-driven UIs. The runner owns a parallel `unordered_map<fixtureId, ActiveLive>`. Each `ActiveLive` holds the current per-channel byte values, a deadline frame, and trigger trace context.

### `ActiveLive` state per fixture
```cpp
struct ActiveLive {
    fixtureId_t fixtureId;
    universe_t  universe;
    uint16_t    channelOffset;
    uint16_t    channelSpan;
    std::vector<uint8_t> values;   // current per-channel values (0 on creation)
    framenum_t  deadlineFrame;     // currentFrame + timeout_ms at last call
    std::string triggerTraceId;    // captured from REST request span
    std::string triggerSpanId;
};
```

### `setLive()` flow

1. Validate up front, *before* touching the map: timeout > 0, values non-empty, every named channel exists on the fixture. Any unknown channel name is a hard failure with no side effects — guards against partial application from typos in a slider UI.
2. If no live entry exists for this fixture: create one with all channels seeded to 0, and **hard-cancel any active pattern** for this fixture (`active_.erase` — no fade-out). Operator wins.
3. If a live entry exists: keep the previous values for channels not named in this call (slider holds the unchanged channels).
4. Apply the resolved `(offset, value)` updates and refresh `deadlineFrame = currentFrame + timeoutMs`.
5. Update trigger trace IDs from the parent span so DMX frames emitted later can be linked back to the request in Honeycomb.

### Tick processing (live entries)

For each `ActiveLive`:
- If `currentFrame >= deadlineFrame`: emit one final blackout frame (zeros across `channelSpan`) and remove the entry. The fixture is now free for patterns again.
- Otherwise: emit the current `values` as a DMXEvent at `(universe, channelOffset)`.

Live entries keep the tick alive — `tick()` returns `!active_.empty() || !live_.empty()`, so the self-scheduling `FixturePatternTickEvent` re-arms while either kind of work is in flight.

### Precedence: live vs patterns

**Live wins.** `start()` (binding-driven and manual `/trigger`) refuses if a live entry exists for the fixture and returns false / 400. `setLive()` cancels the active pattern hard on first arrival. Once the live deadline elapses, patterns can fire again normally.

## Backwards Compatibility

- `Track.fixture_id` defaults to empty in C++ structs and DTOs (`info->required = false`); BSON-schemaless docs missing the field decode cleanly.
- Existing Animation documents continue to play exactly as before. Mixed-fixture animations are new; old ones are unaffected.
- New `fixtures` MongoDB collection — no migration needed.

## Verification

### Unit tests (mirror `tests/creature_test*.cpp` layout)
- `fixture_json_test.cpp` — round-trip JSON ↔ struct; reject missing fields; reject `channel_offset + max(channel.offset) > 511`; duplicate channel names; binding references a nonexistent pattern.
- `fixture_pattern_math_test.cpp` — lerp boundaries: t=0 → startValues; t=fade_in_ms → targetValues; midpoint within ±1; `hold_ms=0` never advances past Hold without explicit stop.
- `fixture_binding_match_test.cpp` — wildcard semantics (nullopt `on_reason` matches all reasons); both-set requires both match.
- `track_dual_id_test.cpp` — Track JSON with neither / both of `creature_id`/`fixture_id` is rejected.
- `tests/fixture/FixturePatternRunner_setLive_test.cpp` — live control validation: success path, rejects empty values / zero timeout / unknown channel name / fixture with no channels; rollback on partial failure (existing session intact, no entry created on fresh failure); subsequent calls extend an existing session. **One test (`RejectsUnknownChannelName`) caught a real bug** during initial implementation — a fresh entry was being created before channel validation, leaving stale state on failure. Now validation happens before any map mutation.

To keep the runner's non-static methods testable without dragging the full event loop into the test target, `tests/server/FakeIdleScheduling.cpp` provides stubs for `EventLoop::scheduleEvent` and `DMXEvent::executeImpl`, and `tests/server/FakeSpans.cpp` stubs `OperationSpan::getTraceIdHex/getSpanIdHex`. The stubs are never invoked by the live-control tests (which only call `setLive`/`hasLive`), but they make the symbols resolvable.

### End-to-end manual test
1. `./local_build.sh`, then run the server.
2. Pick an existing creature (e.g. Beaky) and look up its `id` via `GET /api/v1/creature` — it's a UUID like `1a2b3c4d-5e6f-4789-a0b1-c2d3e4f5a6b7`, not the name.
3. `POST /api/v1/fixture` with a fixture JSON containing a pattern `red-glow` and a binding `{creature_id: "<beaky's id>", on_reason: "ad_hoc", on_state: "running", pattern_id: "<red-glow's id>"}`.
4. `PUT /api/v1/fixture/{id}/universe` with `{universe: 1}`.
5. Trigger an ad-hoc speech for Beaky.
6. Inspect E1.31 output via `sACNView`, `OLA`, or the project's existing DMX debug logging (`DEBUG_DMX_SENDER` in `config.h`).
7. Confirm: fade-in observed → hold during speech → fade-out after the activity transitions back to `Idle`.
8. Restart the server and confirm the universe assignment survives (key persistence check).
9. Hit `POST /api/v1/fixture/{id}/pattern/<red-glow id>/trigger` directly and confirm the pattern fires independently of any creature activity.
10. **Live control smoke test.** `POST /api/v1/fixture/{id}/live` with `{"values":[{"channel":"red","value":255}],"timeout_ms":3000}` and confirm the red channel goes to 255 in your DMX inspector. Within 3s send another live call with `value: 128` and confirm it drops live. Stop sending; ~3s later the channels should black out. While live is active, `POST /pattern/{pid}/trigger` should return 400 / refuse — once the deadline elapses, the same `/trigger` should fire normally.

### Build & lint
- `./local_build.sh` from project root.
- `cd build && ./creature-server-test`.
- Compiles clean under `-Wshadow -Wall -Wextra -Wpedantic`. Watch for shadow warnings: name the dispatcher loop variable `fixture`, not `creature`.

## Suggested Merge Order

Each step is independently mergeable and testable:
1. `DmxFixture` model + DTOs + JSON parsing + Database CRUD + `assigned_universe` field. *(Fixtures storable but inert.)*
2. Controller + service + CRUD + universe PUT/DELETE endpoints + cache invalidation + startup hydration. *(Swift app can fully manage fixtures; DMX output starts working once universe is set.)*
3. `Track.fixture_id` + `PlaybackSession` plumbing + `playback-runner` branch. *(Fixtures in animations.)*
4. `FixturePatternRunner` + `FixturePatternTickEvent` + `FixtureBindingDispatcher` hook in `setActivityState`. *(Triggers fire.)*
5. Manual `/trigger` endpoint, tests, AGENTS.md update.
6. Live control: `ActiveLive` state + `setLive`/`hasLive` on the runner; `SetFixtureLiveRequestDto`; `DmxFixtureService::setFixtureLive`; `POST /api/v1/fixture/{id}/live` endpoint; unit tests for setLive validation. *(Slider UIs can drive raw DMX.)*

---

## Notes for the Swift Client Implementer

The server side is shipped (current version: **3.13.0**, deployed to `https://server.prod.chirpchirp.dev`). The Swift client should implement the JSON Schema Reference above as its data model. Live control and the preview endpoint are the most recent additions; live control is verified end-to-end against real hardware (Light 1: `f2d17206-30b5-4018-b939-daa4b22616c6`). Quick orientation for whoever picks this up next:

### Where to start
1. Read this plan top-to-bottom — the "JSON Schema Reference" is the canonical API contract.
2. Cross-reference `AGENTS.md` "DMX Fixture Model" section for a shorter prose summary of intent.
3. The smoke-test sequence below is a known-working set of payloads.

### Smoke-test payloads (proven against prod)

**Create a fixture** — `POST /api/v1/fixture`, raw JSON body:

```json
{
  "id": "8e3a4b5c-1d2f-4e6a-9b0c-7f8e9d0a1b2c",
  "name": "Stage Left Spot",
  "type": "light",
  "channel_offset": 500,
  "channels": [
    {"offset": 0, "name": "red",        "kind": "color_red"},
    {"offset": 1, "name": "green",      "kind": "color_green"},
    {"offset": 2, "name": "blue",       "kind": "color_blue"},
    {"offset": 3, "name": "brightness", "kind": "master_dimmer"}
  ],
  "patterns": [{
    "id": "7d2a3b4c-5e6f-4789-a0b1-c2d3e4f5a6b7",
    "name": "Red Glow",
    "values": [
      {"channel": "red",        "value": 255},
      {"channel": "brightness", "value": 200}
    ],
    "fade_in_ms": 250,
    "fade_out_ms": 500,
    "hold_ms": 0
  }],
  "bindings": []
}
```

**Assign a universe** — `PUT /api/v1/fixture/8e3a4b5c.../universe` body `{"universe": 1}` (must be in `[1, 63999]`).

**Trigger a pattern manually** — `POST /api/v1/fixture/8e3a4b5c.../pattern/7d2a3b4c.../trigger`, body either empty or `{"stop_after_ms": 1500}` (must be in `(0, 600000]` if provided).

**Preview an unsaved pattern** — `POST /api/v1/fixture/8e3a4b5c.../pattern/preview`, body:

```json
{
  "values": [
    { "channel": "red",        "value": 255 },
    { "channel": "brightness", "value": 200 }
  ],
  "fade_in_ms":  250,
  "fade_out_ms": 500,
  "hold_ms":     0,
  "stop_after_ms": 1500
}
```

The body IS the pattern — `fade_in_ms`, `fade_out_ms`, `hold_ms`, `stop_after_ms` are all optional and default to 0 (snap / hold-forever / no auto-stop). Used by the pattern editor's Fire button so the user can preview unsaved edits without an upsert. Server-side this just constructs an ephemeral `FixturePattern` from the body and hands it to the same runner that handles saved triggers — same validation rules, same live-control preemption, same fade semantics. Nothing is persisted.

**Live control (slider UI)** — `POST /api/v1/fixture/8e3a4b5c.../live`, body:

```json
{
  "values": [
    { "channel": "red",        "value": 255 },
    { "channel": "brightness", "value": 200 }
  ],
  "timeout_ms": 1000
}
```

Pattern for a slider UI: while the user holds a slider, send a live call every ~250 ms with `timeout_ms: 1000` (a safety margin > your send cadence). When the user releases, either stop sending (server auto-blacks out after the last `timeout_ms` elapses) or send one final call with `timeout_ms: 1` to fade out immediately. Channels not named in a call retain the last value you sent for them in the same session, so you only need to include channels that changed. Live takes over the fixture immediately — any pattern that's running is cancelled hard (no fade-out). While live is active, `/trigger` calls and binding-driven patterns are refused; once the deadline elapses, normal patterns work again.

**Practical notes from the prod smoke test on Light 1** (an RGBW + master_dimmer + strobe fixture):

- **Master dimmer needs to be at 255 to see anything.** Color channels are gated by the master on most real fixtures — set `master_dimmer: 255` in your first live call of a session so the colors are visible. Subsequent calls don't need to repeat it (live holds previous values).
- **Strobe is annoying.** Always include `{"channel": "strobe", "value": 0}` in your first live call to make sure the channel isn't carrying garbage from a previous source, or skip strobe controls entirely in the slider UI.
- **Hold time matters for visual confirmation.** Each color needs ~1–2 seconds to register visually. If you flip channels faster than that (sub-100ms), the user will barely see the intermediate colors.
- **400s are returned cleanly with explanatory `message` fields.** Honor them in the UI — unknown channel name, missing/zero/over-cap `timeout_ms`, empty `values`, and no assigned universe all surface useful errors.

**Delete** — `DELETE /api/v1/fixture/8e3a4b5c...` returns `{"status":"OK","code":200,"message":"Fixture deleted","session_id":null}`.

### Suggested UI structure

- **Fixture editor**: form-driven, generates a UUID for `id` client-side. Channel/pattern/binding rows are CRUD lists. Use `type` to pick a renderer (color-picker for `light` with red/green/blue channels; just sliders for `generic` / `smoke_machine` / `fogger`).
- **Universe assignment**: separate from fixture config — a single "Universe" field per fixture row, settable independently. Hitting `PUT /universe` persists it; `null` removes the assignment. After a universe change, listen for `CacheType::Fixture` invalidation broadcasts on the websocket and refresh the local cache.
- **Manual trigger**: a fire button per saved pattern on each fixture. Offer "fire for N seconds" (sends `stop_after_ms`) and "fire and hold" (empty body — caller has to fire a separate stop pattern when done; there's no built-in stop endpoint).
- **Pattern editor "Fire" button**: use the preview endpoint instead of trigger. The editor holds local state that may not match the server's saved fixture; preview ships the on-screen pattern straight to the runner so the user sees their unsaved edits play without a save round-trip. After save, the regular trigger endpoint takes over for "fire the saved version."
- **Live control sliders**: per-channel sliders for ad-hoc lighting. Disable pattern fire buttons on a fixture while the user is actively dragging — pattern triggers are refused by the server during a live session anyway, but disabling them avoids confusing 400s. Re-enable after the deadline elapses (or after the user releases and the auto-blackout fires).
- **Binding editor**: per-fixture list of `{creature_id, on_reason, on_state, pattern_id}`. Use a creature picker (UUID → name from `GET /api/v1/creature`) and a pattern picker scoped to this fixture's patterns.
- **Validation**: hit `POST /api/v1/fixture/validate` before saving. It returns `{valid: bool, fixture_id: string, missing_creature_ids: string[], error_messages: string[]}` — show `missing_creature_ids` as warnings (binding still saves) and `error_messages` as hard blockers.

### Things the server does that aren't visible from the JSON

- **`channel_offset` + `max(channels[].offset)` must be ≤ 511.** If you let users edit channels freely you'll want a live-validation check that won't let them save an overflowing fixture.
- **`bindings[].creature_id` is a soft reference.** You can save a binding pointing at a deleted creature; the dispatcher just won't fire. The validate endpoint flags these so you can warn.
- **`assigned_universe` is persisted** (unlike creatures, where universe is runtime-only). The Swift app does NOT need a "register controller" workflow for fixtures.
- **Cache invalidation events** are broadcast on the websocket after upsert/delete/universe-change as `{"cache_type": "fixture"}`. Consume these to keep the local cache fresh.
- **Pattern triggers are fire-and-forget.** The POST returns the fixture's current state; the actual DMX fade is async on the server's event loop.
- **Live control hard-cancels patterns.** When a `POST /live` lands on a fixture that has an active pattern, the pattern is dropped immediately (no fade-out). New patterns on the same fixture are refused (400) until the live deadline elapses. The Swift client doesn't need to do anything special for this — just be aware that a user dragging sliders preempts everything else on that fixture.

### Server-side files for cross-reference

If the Swift implementer needs to read the source-of-truth shape:

- `src/model/DmxFixture.h` — struct + DTOs
- `src/server/fixture/helpers.cpp` — `fixtureFromJson`, the strict validator
- `src/server/ws/dto/{SetFixtureUniverseRequestDto,TriggerFixturePatternRequestDto,PreviewFixturePatternRequestDto,SetFixtureLiveRequestDto,FixtureConfigValidationDto}.h` — request/response DTOs
- `src/server/ws/controller/DmxFixtureController.h` — endpoint contract
