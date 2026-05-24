# AI Agent Development Guide

This file provides important architectural guidance and context for AI assistants working with this codebase.

---

## Project Overview

Creature Server is a C++ server application for managing April's animatronic creatures. It provides a REST API and WebSocket interface for controlling animations, sounds, playlists, and hardware interactions via DMX/E1.31 lighting protocols.

## Build System

This project uses CMake with Ninja as the build generator and requires C++20. Dependencies are managed via FetchContent for most libraries.

### Key Build Commands

```bash
# Build oatpp dependencies first (required before main build)
./build_oatpp.sh

# Local debug build
./local_build.sh

# Manual build process
mkdir -p build/
cd build/
cmake -DCMAKE_BUILD_TYPE=Debug -G Ninja ..
ninja

# Release build
cmake -DCMAKE_BUILD_TYPE=Release -G Ninja ..
ninja

# Run tests
cd build/
./creature-server-test
# Or via CTest
ctest
```

### Dependencies

External dependencies are fetched automatically via FetchContent:
- MongoDB C/C++ drivers (static linking)
- oatpp web framework (pre-built in externals/)
- spdlog for logging
- OpenTelemetry for observability
- SDL2 for audio
- Opus for audio encoding
- uvgRTP for RTP streaming
- nlohmann/json for JSON handling
- Google Test for unit testing

## Architecture

### Core Components

- **WebSocket Server** (`src/server/ws/`): oatpp-based REST API and WebSocket interface
- **Event Loop** (`src/server/eventloop/`): Main application event processing system
- **Database Layer** (`src/server/database.cpp`): MongoDB integration for data persistence
- **Animation System** (`src/server/animation/`): Animation playback and management
- **Audio/RTP** (`src/server/rtp/`): Real-time audio streaming via RTP/Opus with intelligent caching system
- **GPIO/Hardware** (`src/server/gpio/`): Hardware control interface
- **Metrics** (`src/server/metrics/`): Performance counters and status monitoring

### Data Models

Core entities in `src/model/`:
- **Creature**: Represents an animatronic creature with capabilities
- **Animation**: Motion sequences for creatures
- **Sound**: Audio files and playback metadata
- **Playlist**: Collections of animations/sounds for choreographed shows
- **Track**: Individual timeline elements within playlists

### Services Architecture

The application follows a service-oriented architecture:
- **Controllers** (`src/server/ws/controller/`): HTTP/WebSocket endpoints
- **Services** (`src/server/ws/service/`): Business logic layer
- **DTOs** (`src/server/ws/dto/`): Data transfer objects for API communication

## Critical Architectural Decisions

### Creature Data Model & Universe Separation

**Last Updated:** 2025-10-20

#### Source of Truth Architecture

The creature's JSON configuration file **on the controller** is the source of truth. The MongoDB database is essentially a cache for query convenience. If there's ever a conflict, the controller's config file wins.

#### Creature Configuration vs Universe Assignment

**Important:** These are separate concerns and must be handled differently:

| Aspect | Creature Configuration | Universe Assignment |
|--------|----------------------|---------------------|
| **What is it?** | Servo mappings, capabilities, physical characteristics | Which E1.31 universe the creature listens to |
| **Storage** | Persisted to MongoDB (as cache) | Runtime memory only (`creatureUniverseMap`) |
| **Source of Truth** | Controller's JSON config file | Controller's registration call |
| **Lifetime** | Permanent (creature hardware config) | Ephemeral (exists only while controller is running) |
| **Changes When?** | Hardware modification | Controller moves to different network/setup |

#### Required Creature Fields

All creature JSON configurations must include these fields:

```json
{
  "id": "creature-123",                // Unique identifier
  "name": "Test Creature",             // Display name
  "channel_offset": 0,                 // Offset within universe
  "audio_channel": 1,                  // Audio channel assignment
  "mouth_slot": 5,                     // Slot index for mouth servo (uint8_t)
  "inputs": [...]                      // Optional input mappings
}
```

**Note:** `mouth_slot` was added in October 2025 and is **required**. It defines which slot in the motion array corresponds to the creature's mouth for Rhubarb Lip Sync integration.

#### Controller Registration Endpoint

**Endpoint:** `POST /api/v1/creature/register`

**When to use:** Controllers must call this endpoint when they start up.

**Request:**
```json
{
  "creature_config": "{...creature JSON...}",
  "universe": 1
}
```

**What it does:**
1. Validates the creature JSON schema (including required `mouth_slot` field)
2. Upserts the creature configuration to MongoDB
3. Stores the creature-to-universe mapping in `creatureUniverseMap` (runtime memory)
4. Invalidates client-side creature caches
5. Returns the creature DTO

**Files:**
- `src/server/ws/dto/RegisterCreatureRequestDto.h`
- `src/server/ws/controller/CreatureController.h`
- `src/server/ws/service/CreatureService.h`
- `src/server/ws/service/CreatureService.cpp`

#### Querying Universe for a Creature

```cpp
// Runtime universe lookup
auto universe = creatures::creatureUniverseMap->get(creatureId);
```

This mapping exists only in runtime memory and is populated when controllers register via the `/api/v1/creature/register` endpoint.

#### Physical Reality Modeling

This architecture correctly models the physical system:
- Creatures are physical hardware that can be unplugged and moved
- Universe assignment happens when the controller boots up and registers
- The same creature config works regardless of which universe it's assigned to
- Universe state is ephemeral (lost on server restart), which is correct behavior

### DMX Fixture Model

**Last Updated:** 2026-05-21

#### What's a DmxFixture?

A `DmxFixture` is a first-class non-Creature DMX device — lights, smoke machines, foggers, etc. Where creatures have **axes** (servo slots), fixtures have **channels** (named DMX byte slots: `red`, `green`, `brightness`, `fog_output`, …). The data model and API are intentionally generic so the same code path serves new device types as needed.

#### Intentional Divergence from Creatures

| Aspect | Creature | DmxFixture |
|--------|----------|-----------|
| **Source of truth** | Controller's JSON config file | Server's MongoDB |
| **Managed by** | Controller pushes config on startup via `/register` | Creature Console Swift app via REST CRUD |
| **Universe assignment** | Ephemeral (`creatureUniverseMap`, runtime only) | **Persisted** as `assigned_universe` on the fixture document, mirrored to `fixtureUniverseMap` at startup |
| **`register` endpoint** | Yes (controller bootstrap) | No (no controller in the loop) |

Fixtures are usually stage hardware wired to a fixed DMX address, so losing the universe assignment on every server restart would mean the user manually re-binds every light. Persisting it eliminates that friction.

#### JSON Schema (API contract)

Submitted by the Creature Console Swift app via `POST /api/v1/fixture`. **All IDs are UUIDs** (RFC 4122), never MongoDB OIDs.

```json
{
  "id": "8e3a4b5c-1d2f-4e6a-9b0c-7f8e9d0a1b2c",
  "name": "Stage Left Spot",
  "type": "light",                          // "light" | "smoke_machine" | "fogger" | "generic"
  "channel_offset": 500,
  "assigned_universe": 1,                   // nullable
  "channels": [
    { "offset": 0, "name": "red",        "kind": "color_red" },
    { "offset": 1, "name": "green",      "kind": "color_green" },
    { "offset": 2, "name": "blue",       "kind": "color_blue" },
    { "offset": 5, "name": "brightness", "kind": "master_dimmer" }
  ],
  "patterns": [
    {
      "id": "7d2a3b4c-5e6f-4789-a0b1-c2d3e4f5a6b7",
      "name": "Red Glow",
      "values": [{ "channel": "red", "value": 255 }, { "channel": "brightness", "value": 200 }],
      "fade_in_ms": 250,
      "fade_out_ms": 500,
      "hold_ms": 0                          // 0 = hold until external stop
    }
  ],
  "bindings": [
    {
      "creature_id": "1a2b3c4d-5e6f-4789-a0b1-c2d3e4f5a6b7",
      "on_reason":   "ad_hoc",              // null = wildcard
      "on_state":    "running",             // null = wildcard
      "pattern_id":  "7d2a3b4c-5e6f-4789-a0b1-c2d3e4f5a6b7"
    }
  ]
}
```

**Validation rules** (server enforces, 400 on failure): `channel_offset + max(channels[].offset) ≤ 511`; channel names unique; pattern ids unique; pattern `values[].channel` references existing channel; binding `pattern_id` references existing pattern; `on_reason`/`on_state` parse to known enums. `creature_id` is a soft reference — not validated against the creature database.

**Channel `kind` values** (UI hint only; server never branches on them): `color_red`, `color_green`, `color_blue`, `color_white`, `color_amber`, `color_uv`, `master_dimmer`, `strobe`, `pan`, `tilt`, `gobo`, `generic`. New values can be added without server changes.

#### REST Endpoints

```
GET    /api/v1/fixture                                       list all
GET    /api/v1/fixture/{fixtureId}                           get one
POST   /api/v1/fixture                                       create or update
DELETE /api/v1/fixture/{fixtureId}                           delete
POST   /api/v1/fixture/validate                              validate without saving

PUT    /api/v1/fixture/{fixtureId}/universe                  body: {universe: UInt32}
DELETE /api/v1/fixture/{fixtureId}/universe                  clear assignment

POST   /api/v1/fixture/{fixtureId}/pattern/{patternId}/trigger
                                                             body (optional): {stop_after_ms: UInt32}

POST   /api/v1/fixture/{fixtureId}/pattern/preview           fire an ephemeral, not-persisted pattern
                                                             body: {values, fade_in_ms?, fade_out_ms?, hold_ms?, stop_after_ms?}

POST   /api/v1/fixture/{fixtureId}/live                      body: {values: [{channel, value}], timeout_ms: UInt32}
                                                             slider-driven raw DMX with auto-blackout
```

#### Three Ways to Control Fixtures

1. **Animation tracks.** `Track` has `fixture_id` alongside `creature_id` — exactly one is set per track. Mixed creature + fixture animations work fine; the playback runner branches on `trackState.fixtureId` and resolves the universe via `fixtureUniverseMap` (independent of the animation session's universe).
2. **Patterns + bindings.** A `FixturePattern` is a named snapshot of channel values with fade-in / hold / fade-out (`hold_ms=0` means "hold until external stop"). A `FixtureBinding` declaratively says *"when creature X enters activity (reason, state), apply pattern P."* Bindings live on the fixture config — fixtures are self-describing.
3. **Live control.** `POST /api/v1/fixture/{id}/live` drives channels directly from a slider UI. The server holds the values until `timeout_ms` elapses (≤ 10 min cap), then blacks out. **Live wins**: starting live hard-cancels any active pattern on the fixture (no fade-out), and new patterns are refused while live is in effect. Channels not named in a call retain their previous live value (or 0 on the first call). Caller must always specify `timeout_ms` (no default) — guards against a disconnected client leaving the light stuck.

#### Runtime Pieces

- **`FixturePatternRunner`** owns `unordered_map<fixtureId_t, ActivePattern>`. `start()` does a **last-wins smooth handoff** (current rendered bytes become the new `startValues` — no DMX snap). `stop()` transitions to FadeOut.
- **`FixturePatternTickEvent`** is a self-scheduling event that reschedules every **20 ms (~50 Hz)** while any patterns are active. Pauses when the map is empty; re-armed by the dispatcher on the next trigger.
- **`FixtureBindingDispatcher`** is edge-triggered. Called from `CreatureService::setActivityState` via the `fixtureActivityHook` `std::function`. Tracks `(lastReason, lastState)` per creature so identical re-assertions are no-ops. When a binding starts matching: `runner->start()`. When it stops matching: `runner->stop()`.
- **`fixtureActivityHook`** decouples `CreatureService` from the fixture subsystem at link time. Main binary installs the hook; tests leave it empty.

#### Files Map

- **Model**: `src/model/DmxFixture.{h,cpp}`, `src/model/Track.{h,cpp}` (with `fixture_id`)
- **DB**: `src/server/fixture/{helpers,upsert,get,getall}.cpp`
- **REST**: `src/server/ws/controller/DmxFixtureController.h`, `src/server/ws/service/DmxFixtureService.{h,cpp}`, `src/server/ws/dto/{FixtureConfigValidationDto,SetFixtureUniverseRequestDto,TriggerFixturePatternRequestDto,PreviewFixturePatternRequestDto,SetFixtureLiveRequestDto}.h`
- **Runtime**: `src/server/fixture/{FixturePatternRunner,FixturePatternTickEvent,FixtureBindingDispatcher}.{h,cpp}`
- **Hook**: `src/server/ws/service/FixtureActivityHook.h`
- **Globals**: `src/server/main.cpp` (`fixtureCache`, `fixtureUniverseMap`, `fixturePatternRunner`, `fixtureBindingDispatcher`, `fixtureActivityHook`)
- **Collection**: `"fixtures"` in `creature_server` (`FIXTURES_COLLECTION` in `config.h`)

### Event Loop System

#### Critical Timing Requirements

**⚠️ CRITICAL: The event loop MUST run every 1ms exactly.** This precise timing is fundamental to the entire system's operation and cannot be compromised. Any modifications to event loop code must preserve this exact 1ms interval.

#### Event Loop Tracing System

The server implements intelligent selective tracing for the high-frequency event loop to provide error visibility while controlling observability costs:

**Key Features:**
- **Selective Sampling**: Configurable sampling rate for normal event loop frames (default: 0.1% = 1 in 1000)
- **Error-First Export**: All errors and exceptions are always traced regardless of sampling rate
- **Smart Export Logic**: Only exports traces when errors occur or random sampling criteria met
- **Rich Telemetry**: Frame numbers, events processed, queue sizes, timing data

**Configuration:**
- **Environment Variable**: `EVENT_LOOP_TRACE_SAMPLING` (0.0 to 1.0)
- **Command Line**: `--event-loop-trace-sampling 0.01` (for 1% sampling)
- **Default**: 0.001 (0.1% sampling rate)

**Implementation:**
- **Class**: `SamplingSpan` provides conditional export logic
- **Integration**: Event loop creates sampling spans for each frame iteration
- **Export Control**: Spans marked with sampling metadata for filtering in Honeycomb
- **Error Detection**: Automatic promotion to always-export on exceptions

**Cost Benefits:**
- **Volume Reduction**: 99.9% reduction in normal trace volume
- **Error Coverage**: 100% of errors and exceptions captured
- **Performance**: Minimal overhead (~1μs per frame for sampling decision)
- **Honeycomb Cost**: Dramatic reduction in trace ingestion costs

### Audio Cache System

The server includes an intelligent caching mechanism for pre-encoded Opus files to dramatically improve performance:

**Key Features:**
- **Performance**: Reduces Opus encoding time from dozens of seconds to <20ms for cache hits
- **Smart Invalidation**: Uses SHA-256 checksums, file modification times, and file sizes for cache validation
- **Storage Format**: Binary format with embedded metadata for fast validation
- **Location**: Stored in `.opus_cache/<hostname>/` subdirectory within the configured sounds directory
- **Multi-Machine Safe**: Each machine gets its own cache directory based on hostname to prevent conflicts on shared storage

**Implementation Details:**
- **Class**: `util::AudioCache` provides the caching interface
- **Integration**: `AudioStreamBuffer` automatically uses cache when available
- **File Format**: Custom binary format (17 channels × N frames) with metadata header
- **Error Handling**: Graceful fallback to direct encoding if cache unavailable or corrupt

**Performance Impact:**
- **Raspberry Pi 5**: Encoding time reduced from 30+ seconds to milliseconds
- **Cache Hit Rate**: High for repeated audio file usage in shows/playlists
- **Memory Usage**: Minimal - cache files stored on disk, loaded on demand

## Development Workflow

### Testing

Unit tests are located in `tests/` and use Google Test framework. The test executable is `creature-server-test`.

### Linting and Code Quality

The project enforces strict compiler warnings:
- `-Wshadow` (overshadowed declarations)
- `-Wall -Wextra -Wpedantic`

**Code Formatting**: Always run `clang-format` on modified files. The project includes a `.clang-format` configuration file in the root directory that defines the required formatting style.

### Required PR Reviews

**Every PR must receive two specialized AI reviews before merge.** Run both in parallel — they're independent and a single `Agent` tool call message with two invocations gets them in flight at once.

#### 1. Adversarial security review

Launch a `general-purpose` subagent framed as an **adversarial** reviewer (not a friendly one). Brief it with:

- The commit range under review (`git log --oneline <base>..<head>`).
- The threat model: local-network attacker, malicious/buggy Swift client, authorized user accidentally causing harm. There is no external auth layer in front of any existing endpoint — flag if NEW endpoints behave worse than existing ones under attack, don't waste cycles on "add OAuth."
- The new attack surface (REST routes, JSON parsing, BSON writes, runtime caches, event-loop work).

Things the reviewer should actively probe:
- Input validation gaps in JSON parsing (`get<T>()` vs `value("k", default)`, presence-only vs strict, integer ranges, string length, type confusion).
- NoSQL/MongoDB injection — are user-supplied strings interpolated into BSON queries? Are JSON fields stored as-is, and could that re-enter a query later?
- DoS / resource exhaustion — unbounded arrays, unbounded loops, unpaginated reads, event-loop priority-queue growth, far-future scheduled events.
- TOCTOU and race conditions — concurrent transitions, cache lookups split across `contains`/`get`, multi-threaded global maps.
- Authorization-style logic bugs — can a fixture/binding/pattern clobber output that should belong to a different creature or universe?
- Crash vectors — uncaught exceptions on the event-loop thread (these are catastrophic), `std::out_of_range` from `ObjectCache::get`, `nlohmann::json::type_error` paths.
- Path-parameter values flowing into log lines and span attributes unsanitized.

The reviewer should produce **5–15 findings, sorted by severity** (Critical / High / Medium / Low / Informational), each with: location (file:line), concrete attacker scenario, why it works, and a specific fix (not "add validation" — say which one).

#### 2. OTel / observability review

Launch the `honeycomb:instrumentation-advisor` subagent. Brief it with:

- The commit range under review.
- The location of the project's instrumentation wrappers (`ObservabilityManager::createRequestSpan` / `createOperationSpan` / `createChildOperationSpan` / `createSamplingSpan`) and the existing **Creature subsystem as the gold-standard baseline** to compare against (`src/server/ws/controller/CreatureController.h`, `src/server/creature/{helpers,upsert,get,getall}.cpp`, `src/server/ws/service/CreatureService.cpp`).
- High-frequency code paths and whether they should use the `SamplingSpan` infrastructure (see "Event Loop Tracing System" section). Default sampling rate: `0.0005` (0.05%).

Things the reviewer should actively probe:
- **Span coverage** — every REST endpoint has a request span; every service method has an operation span; every DB call has a child span. Critical untraced paths flagged.
- **Attribute richness** — Honeycomb's value comes from wide events with high cardinality. UUIDs, names, sizes, counts, enum strings should all be on spans. Anything you'd want to GROUP BY in a query.
- **Attribute naming consistency** — `dot.separated.lowercase`, semantic conventions for HTTP/DB/error, no two attribute names for the same business concept.
- **Error recording** — `setError(msg)` + `error.type` + `error.code` + `error.message` + `recordException` on the throw paths.
- **Span lifecycle / hierarchy** — parent spans threaded through every level, `setSuccess()` on the happy path, no spans accidentally orphaned.
- **`setHttpStatus` placement** — must be reached on the error path too, not just the success return. The recurring bug pattern in this codebase is `setHttpStatus(200)` after the service call, with no try/catch around it — exception unwinding skips the status set.
- **High-frequency code** — does it use `createSamplingSpan` with a sensible rate, or is it untraced / over-traced?
- **Cross-system tracing** — async work (scheduled events, ticks driven by binding dispatcher) should carry trace context back to the originating request span via span links or `trigger.trace_id` / `trigger.span_id` attributes.

The reviewer should produce a report with: one-paragraph health rating, "what's done well" (give credit), **gaps sorted by impact on observability quality** (each with file:line, what's missing, concrete query that's currently unanswerable, suggested fix), cross-cutting recommendations, and a final section on high-frequency-path strategy.

#### When to skip

Skip both reviews only if the diff is purely cosmetic (typos, formatting, comments) or touches no behavior. Version bumps and CMakeLists-only changes do not need either review. Doc-only changes to AGENTS.md or `plan/` do not need either review. Everything else gets both.

### Observability

Built-in OpenTelemetry integration for traces and metrics. The `ObservabilityManager` handles telemetry configuration and export.

## Key Libraries and Conventions

### Custom Libraries

- **CreatureVoicesLib** (`lib/CreatureVoicesLib/`): Voice synthesis integration
- **e131_service** (`lib/e131_service/`): DMX/E1.31 lighting protocol support
- **libe131** (`lib/libe131/`): Core E1.31 implementation

### Threading and Concurrency

Uses `moodycamel::ConcurrentQueue` for thread-safe message passing between components. The event loop pattern centralizes most application logic.

### Configuration

Command-line arguments handled via argparse. Database connection and server configuration managed through environment variables and config classes.

## Platform Notes

- **macOS**: Uses system SDL2 and resolv libraries
- **Debian Linux**: Additional UUID library dependency via pkg-config; OpenSSL 3.x (libssl3) required for audio cache hashing
- **Raspberry Pi**: Specialized build scripts (`pi4_build.sh`); runs on Raspberry Pi OS (Debian-based)

## Deployment

Docker-based deployment with multi-architecture support (AMD64/ARM64). GitHub Actions handle automated builds and container publishing.

## Important Notes for AI Agents

1. **All entity IDs are UUIDs** - `Creature.id`, `Animation.id`, `Track.id`, `DmxFixture.id`, `FixturePattern.id`, etc. are all RFC 4122 UUIDs. This project never uses MongoDB OIDs. Existing comments saying "MongoDB OID" are stale.
2. **Always require `mouth_slot`** - All creature configurations must include this field (uint8_t)
3. **Never store universe in Creature model** - Universe is runtime state only for creatures, stored in `creatureUniverseMap`. *Fixtures are different — see DMX Fixture Model section.*
4. **Controller registration is the entry point for creatures** - Controllers must call `/api/v1/creature/register` on startup. Fixtures do not have a registration step (managed entirely from the Swift app via REST CRUD).
5. **Creature DB is a cache; fixture DB is the source of truth** - Don't rely on database as source of truth for creature configs (controller's JSON file is). For fixtures, MongoDB is authoritative.
6. **Track must have exactly one of `creature_id` / `fixture_id`** - Setting both, or neither, is a 400 validation error.
7. **Event loop timing is sacred** - Never modify code that could affect the 1ms event loop interval
8. **Code formatting matters** - Always use clang-format before committing

## Future Work

### DmxFixture follow-up work

Items called out by the post-merge security + OTel reviews of the DmxFixture feature that should land in a follow-up PR (none are critical bugs — they're architectural gaps and observability polish):

- **Animation-vs-pattern DMX precedence (security H4)**. The plan said "animation tracks win" but it's not implemented. Both `FixturePatternRunner::tick` and `PlaybackRunnerEvent::emitDmxFrames` enqueue DMXEvents to the same loop with no merge step; the on-wire byte is whichever event the loop processes last. A fixture whose `(universe, channel_offset..+span)` overlaps a creature's animation channels can stomp the animation. Same overlap concern applies to live control vs animations (no merge there either — though in practice live control is operator-driven and short-lived). Cheap fix: refuse to `start()` a pattern (or `setLive()`) on a fixture that overlaps an active session's track. Proper fix: per-frame DMXEvent merge with documented priority.
- ~~**`runEndpoint(...)` helper for controllers (OTel P1c)**.~~ Done in `audit/security-and-observability` (2026-05-23). Helper lives in `src/server/ws/controller/ControllerUtils.h` and is used by Playlist/Voice/Debug/Metrics/Static controllers. The Creature/Animation/Sound/SpeechToText/StreamingAdHoc controllers still use the old explicit pattern with `setHttpStatus(200)` outside try/catch and should be migrated to `runEndpoint` for consistency — see `docs/audit-2026-05-23.md` finding O-H2.
- **Pagination on `GET /api/v1/fixture`**. Same as the creature endpoint shape, not urgent on a home network.
- **`error.type` Honeycomb column is sticky-bool.** Pre-3.11 writes established this as a boolean in Honeycomb's type inference; every string write (e.g. `error.type = "UnknownChannel"`) is now recorded as `false` in the UI. The const char* overload fix is working (other string columns like `error.channel_name` record correctly). Fix needs Honeycomb-side action: delete the column in the dataset schema so it re-types on next write, or rename project-wide to a new attribute key. 40+ call sites — separate PR. ~~`http.flavor`~~ has been renamed to `http.protocol_version` in 3.13.3 (it had the same issue but only one call site, so a rename was easy).
- **UUID-shape check on path params (security M4)**. Already added on fixture controllers, but the same hardening should apply to the existing creature controller for log-injection resistance.

### Rhubarb Lip Sync Integration

The `mouth_slot` field is prepared for integration with Rhubarb Lip Sync. When that feature is implemented:
- The server can automatically generate mouth movement data from audio files
- The generated mouth positions will be applied to the `mouth_slot` index in the animation frames
- API endpoint exists at `/api/v1/sound/generate-lip-sync` (see `GenerateLipSyncRequestDto.h`)
- Implementation files: `src/server/voice/RhubarbData.h`, `src/server/voice/RhubarbData.cpp`

---
