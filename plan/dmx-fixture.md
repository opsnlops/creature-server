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
| `assigned_universe` | uint32 \| null | no | `null` | Persisted DMX universe assignment (E1.31 universe number). `null` means the fixture is configured but produces no DMX output. Settable via `PUT /api/v1/fixture/{id}/universe`. |
| `channels` | array of `FixtureChannel` | yes | — | Non-empty. Defines the addressable channels of this fixture. |
| `patterns` | array of `FixturePattern` | no | `[]` | Named DMX value snapshots that bindings can trigger. |
| `bindings` | array of `FixtureBinding` | no | `[]` | Declarative triggers connecting creature activity transitions to patterns on this fixture. |

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
| `values` | array of `FixturePatternValue` | yes | — | Target channel values. Channels not listed are not driven by this pattern (preserve whatever the previous render had). |
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

The validate-only endpoint (`POST /api/v1/fixture/validate`) returns these errors structured for UI display rather than a single 400 string.

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
| `src/server/ws/dto/FixtureConfigValidationDto.h` | Validation response | `CreatureConfigValidationDto.h` |
| `src/server/fixture/FixturePatternRunner.{h,cpp}` | Active-pattern map + lerp engine | (new) |
| `src/server/fixture/FixturePatternTickEvent.{h,cpp}` | Eventloop tick (~50 Hz) for fade interpolation | (new) |
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
- `POST   /api/v1/fixture/{fixtureId}/pattern/{patternId}/trigger` — manually fire a pattern (skips binding match). Body optional `{stop_after_ms?: number}`.

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
