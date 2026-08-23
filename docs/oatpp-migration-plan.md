# Oat++ migration: framework-neutral data contracts and replaceable transport

**Status:** Tracking
**Master issue:** [#162](https://github.com/opsnlops/creature-server/issues/162)
**DTO inventory:** [oatpp-dto-inventory.md](https://github.com/opsnlops/creature-server/blob/main/docs/oatpp-dto-inventory.md)

## Goal

Remove oat++ from Creature Server's data contracts, domain models, services,
and WebSocket messages before replacing the HTTP/WebSocket transport. The
result must let the server move to Drogon, Crow, RESTinio, or another transport
without another application-wide model rewrite.

The first and most important boundary is:

> Services return domain values or framework-neutral request/response structs.
> They do not return oat++ objects, and they normally do not return untyped JSON.

`nlohmann::json` is the wire-format implementation. Ordinary C++ structs remain
the typed contract within the application.

## Why this is a separate project from the server replacement

Oat++ is currently used far beyond socket handling:

- 106 REST endpoints use oat++ controller macros.
- 131 DTO/envelope classes contain 479 `DTO_FIELD` declarations.
- 176 source, test, and `CreatureVoicesLib` files refer to oat++; 167 are in
  Creature Server's `src` and `tests` trees.
- Domain headers in `src/model/` define oat++ DTOs next to the real models.
- Nine service headers expose oat++ types.
- WebSocket envelopes, commands, and payloads use the oat++ object mapper.
- `CreatureVoicesLib` exposes oat++ DTOs from its public model headers.

Changing the HTTP framework first would therefore combine two independent
risks: changing protocol/runtime behavior and changing every data contract.
Neutralizing the contracts while the existing oat++ server still runs makes
the later transport replacement much smaller and easier to verify.

## Scope

### In scope

- Framework-neutral domain and API types.
- Explicit `nlohmann::json` parsing and serialization.
- Strict request validation, including type, presence, range, UUID, array-size,
  and unknown-field rules.
- Neutral service inputs and return values.
- Neutral REST request and response contracts.
- Neutral WebSocket envelopes, commands, and payloads.
- Neutral public models in `CreatureVoicesLib`.
- Temporary oat++ adapters that keep the current server operational.
- Contract, round-trip, negative-validation, and differential tests.
- A boundary check preventing oat++ from returning to neutral layers.
- Replacement of the HTTP/WebSocket transport after DTO removal is complete.

### Out of scope for the DTO phases

- Choosing the final HTTP framework before the transport spike is complete.
- Rewriting HTTP parsing, keep-alive, WebSocket framing, or socket handling.
- Creating a general-purpose reflection or web framework.
- Preserving Swagger/OpenAPI generation. Creature Server controls its only
  client and does not require generated OpenAPI.
- Unrelated changes to the 1 ms creature event loop.

## Target architecture

```text
Drogon / Crow / RESTinio / current oat++ adapter
                         |
                         v
              Creature transport adapter
       HTTP request/response + WebSocket sessions
                         |
                         v
        framework-neutral API request/response structs
                         |
                         v
                services returning Result<T>
                         |
                         v
                    domain models
```

Proposed ownership:

```text
src/model/                       domain structs; no transport types
src/api/request/                 plain REST/WebSocket request structs
src/api/response/                plain response-only structs
src/api/json/                    checked extraction and JSON codecs
src/server/ws/oatpp/             temporary oat++ transport integration
src/server/ws/oatpp/legacy_dto/  temporary adapters removed during migration
```

Exact directory names can change during the first implementation PR, but the
dependency direction may not: models and services must not depend on a web
framework.

## Data-contract rules

### Typed values inside the application

Use domain models where the API and domain concepts are the same. Introduce a
plain request or response struct when the wire contract does not naturally map
to a domain entity.

Services should use shapes such as:

```cpp
Result<Animation> getAnimation(const std::string &animationId);
Result<std::vector<DmxFixture>> getAllFixtures();
Result<void> stopPlaylist(universe_t universe);
Result<std::optional<Creature>> findCreature(const creatureId_t &creatureId);
```

Services must not return HTTP status codes or build HTTP error envelopes. The
transport maps `ServerError` to the appropriate status and JSON response.

### Explicit codecs

Prefer explicit codec functions over `NLOHMANN_DEFINE_TYPE_*` macros for public
contracts:

```cpp
nlohmann::json animationToJson(const Animation &animation);
Result<Animation> animationFromJson(const nlohmann::json &json);

Result<PlayAnimationRequest>
playAnimationRequestFromJson(const nlohmann::json &json);
```

The contracts contain enough conditional requirements, bounded integers,
mutually exclusive fields, and opaque extension points that generated field
mapping would conceal important behavior.

### Validation policy

- Required means present, non-null, and the correct JSON type.
- Optionality is represented with `std::optional<T>` where absence is meaningful.
- Wrong types are errors; no string/number/bool coercion.
- Integer values are checked against the destination type and business range.
- Entity IDs are checked as RFC 4122 UUIDs where the existing API requires UUIDs.
- Arrays have explicit size caps before allocation or iteration.
- Unknown request fields are rejected by default.
- Forward-compatible opaque objects, such as storyboard actions, explicitly
  opt out of unknown-field rejection.
- JSON exceptions are caught at the codec boundary and converted to
  `ServerError::InvalidData`; they must not escape to the event loop.
- Serialization spells wire keys explicitly, using the current snake_case names.
- Generic list responses derive `count` from `items.size()`.

### Missing versus null

The existing oat++ serializer emits explicit `null` for many unset wrapper
fields. Some tests and parsers currently accommodate that behavior. Before
converting each contract, classify every nullable field as one of:

1. required and non-null;
2. optional and omitted when absent;
3. nullable, where explicit `null` carries meaning.

The canonical contract omits absent optional values. We will not reproduce
oat++'s accidental explicit-null wrapper behavior. Each affected shape is
tested and coordinated with the Swift client; see Creature Console issue #83
for the first Track/Animation slice.

### API parsing versus persistence parsing

`JsonParser::parseJsonString` currently reports malformed JSON as a database
error. The API layer needs a separate entry point, or a configurable error
classification, that returns `InvalidData` for client input. Database/BSON
conversion errors must retain their existing database classification.

## Compatibility strategy

The DTO migration does not require changing the running HTTP server. Existing
oat++ endpoints can temporarily accept `BODY_STRING`, parse it through the
neutral codec, call a neutral service, and return `json.dump()` in an oat++ raw
response.

Where converting a controller immediately would make a model extraction too
large, move its oat++ DTO into `legacy_dto/` and implement it as an adapter over
the neutral model. Legacy adapters are migration scaffolding: new code may not
depend on them, and each must have a named removal milestone.

## Work plan

### Phase 0 — Inventory and contract characterization

- [x] Produce a complete DTO inventory grouped by domain model, REST request,
      REST response, WebSocket inbound, WebSocket outbound, and external-client
      adapter.
- [x] Record every service method whose public signature contains an oat++ type.
- [x] Record non-web runtime code that consumes or produces DTOs.
- [ ] Add representative golden JSON tests for each contract family.
- [ ] Add negative tests for missing, null, wrong-type, overflow, oversized
      array, and unknown-field inputs.
- [ ] Mark every observed wire-shape difference as preserve, intentionally
      change, or internal-only.
- [ ] Record the matching Swift-client change for every intentional wire change.

**Exit criterion:** the migration has a complete checklist and serialization
changes cannot occur silently.

### Phase 1 — Neutral JSON codec foundation

- [x] Add small checked-extraction helpers for required/optional fields,
      bounded integers, UUIDs, arrays, and object allowlists.
- [x] Establish consistent field-path error messages.
- [x] Add an API JSON parser that returns `InvalidData` without changing BSON
      error behavior.
- [x] Add neutral JSON response helpers for the canonical status envelope and
      list responses.
- [x] Document the codec conventions in code and tests.

The checked-extraction foundation and neutral response helpers are proven by
the Track/Animation slice. The conventions are recorded in
`docs/json-codec-conventions.md` for subsequent resource families.

**Exit criterion:** a new request or response can be implemented without an
oat++ DTO and without hand-writing repetitive unsafe `json.get<T>()` calls.

### Phase 2 — Remove oat++ from domain models

Migrate leaf models before aggregate models.

- [x] `Input`
- [x] `Track`
- [x] `AnimationMetadata` and render-choice types
- [x] `PlaylistItem`
- [x] `Notice`
- [x] `LogItem` and log-level serialization
- [x] `CacheInvalidation`
- [x] `VirtualStatusLights`
- [x] `StreamFrame`
- [x] `PlaylistStatus`
- [x] `Animation`
- [x] `Creature` configuration (runtime state remains a separate snapshot-concurrency checkpoint)
- [x] `Playlist`
- [ ] `Sound` and nested timing/cue types
- [ ] `DmxFixture` and nested channel/pattern/binding types
- [ ] `DialogScript` and nested accepted-voice/music types
- [ ] `AdHocExchange`
- [x] `Stage`
- [x] `Storyboard`
- [ ] Move remaining oat++ DTOs into temporary transport adapters or remove them.
- [ ] Remove oat++ includes and code-generation macros from `src/model/`.

The `Input` checkpoint is structural: it defines the strict neutral leaf
contract, but the active Creature request parser still uses its legacy inline
conversion. The Creature checkpoint must adopt `inputFromJson`, add a request
body limit and input-count limit, and validate aggregate constraints that no
single input can decide: occupied slot range, duplicate names, and overlapping
slot ranges. Storage-width maxima in the leaf codec are not a substitute for
those Creature-level physical constraints.

The `PlaylistItem` checkpoint is likewise structural until the Playlist
aggregate adopts it. Its canonical contract requires an animation UUID and a
weight from 1 through 999. The Playlist checkpoint must cap item count, reject
duplicate animation IDs, derive or verify `number_of_items`, bound total
weight, and activate the leaf parser. Playback already uses checked cumulative
weighted selection rather than allocating one string per unit of weight. The
matching Swift validation cleanup is tracked in `creature-console#84`.

For every model:

1. add or audit its neutral serializer;
2. add a checked parser where inbound conversion is supported;
3. test all required/optional/nullable fields;
4. test round trips only where round-trip symmetry is part of the contract;
5. update old endpoints through a temporary adapter if necessary.

**Exit criterion:** `rg 'oatpp|OATPP' src/model` returns no matches.

### Phase 3 — Neutralize service interfaces

Convert one vertical resource family at a time. Each slice includes the service
header, implementation, controller adaptation, and tests.

- [ ] Fixtures
- [ ] Creatures
- [x] Animations and ad-hoc animations
- [ ] Playlists and playlist status
- [ ] Sounds and renditions
- [ ] Dialog scripts, preview, voice acceptance, and music
- [ ] Voice generation and subscription state
- [ ] Metrics, jobs, and runtime status
- [ ] Remove oat++ assertions and HTTP status construction from services.
- [ ] Return `Result<T>`, `Result<void>`, vectors, optionals, and domain values.
- [ ] Accept `std::string`, strong IDs, domain values, and neutral request structs.

**Exit criterion:** `rg 'oatpp|OATPP' src/server/ws/service` returns no matches.

### Phase 4 — Replace REST DTOs

- [ ] Convert request DTOs under `src/server/ws/dto/` to plain structs and
      checked neutral parsers.
- [ ] Convert response DTOs to domain serializers or plain response structs.
- [ ] Replace `BODY_DTO` endpoints with raw-body parsing where needed.
- [ ] Replace `createDtoResponse` with canonical raw JSON responses.
- [ ] Preserve body-size limits and strict unknown-field behavior.
- [ ] Preserve request-span creation, HTTP attributes, status recording, and
      exception/error recording.
- [ ] Remove generic oat++ `ListDto` types.
- [ ] Remove the oat++ JSON object mapper from REST handling.

**Exit criterion:** all REST request and response bodies are parsed and rendered
by the neutral JSON layer; oat++ is only routing and transporting bytes.

### Phase 5 — Replace WebSocket DTOs

- [x] Define a neutral `{command, payload}` envelope.
- [x] Parse the envelope once and dispatch by command.
- [ ] Parse each inbound payload into a command-specific plain struct.
      Notice, stream-frame, board/motor sensor-report, and Dynamixel are complete.
- [x] Convert stream-frame, notice, sensor-report, and Dynamixel commands.
- [ ] Convert cache invalidation, activity, jobs, logs, counters, playlist
      status, stream frames, notices, and status-light outbound messages.
      Cache invalidation, activity, jobs, logs, counters, playlist status,
      notices, and status lights are complete. Stream frames have no active
      outbound path; their unused oat++ wrapper has been removed.
- [ ] Preserve message and aggregate-array size caps.
- [ ] Preserve fragmented-message handling and malformed-message isolation.
- [ ] Change message handler interfaces from `oatpp::String` to `std::string_view`
      or another lifetime-safe standard type.
- [ ] Remove the oat++ object mapper from WebSocket handling.

**Exit criterion:** WebSocket business code only sends and receives standard C++
types and serialized JSON strings.

### Phase 6 — Neutralize CreatureVoicesLib public models

- [ ] Convert `Voice`.
- [ ] Convert `Subscription`.
- [ ] Convert creature-speech request and response models.
- [ ] Keep any temporary oat++ HTTP-client conversion inside the client
      implementation, not public model headers.
- [ ] Update Creature Server's voice service to consume neutral library types.

**Exit criterion:** public `CreatureVoicesLib` model headers contain no oat++
types, includes, or macros.

### Phase 7 — Enforce the boundary and remove DTO infrastructure

- [ ] Add a CI/build check that forbids oat++ references in `src/model`,
      `src/api`, services, messaging business logic, and public voice models.
- [ ] Delete all temporary `legacy_dto/` adapters.
- [ ] Delete obsolete DTO conversion functions and DTO-only tests.
- [ ] Retain or replace golden JSON and validation tests as permanent contract
      coverage.
- [ ] Remove oatpp-swagger and its packaged resources.
- [ ] Remove the oat++ object mapper and DTO code-generation dependencies.
- [ ] Run clang-format on all modified C++ files.
- [ ] Run the full native test suite on macOS and Linux ARM64/AMD64.

**Exit criterion:** oat++ references are confined to the HTTP/WebSocket transport
implementation that is about to be replaced.

### Phase 8 — Select and replace the transport

- [ ] Implement matching Drogon and Crow spikes with health, one representative
      CRUD slice, one large/range file response, WebSockets, OTel propagation,
      and graceful shutdown.
- [ ] Test malformed bodies, keep-alive, bodies on GET/DELETE, HEAD, byte ranges,
      slow WebSocket clients, fragmentation, cross-thread broadcast, and shutdown.
- [ ] Measure build impact, idle resource use, request latency, and any effect on
      the independent 1 ms creature event loop.
- [ ] Record the framework decision and rationale.
- [ ] Implement the selected transport adapter against the already-neutral API.
- [ ] Remove oat++, oatpp-websocket, oatpp-swagger, `build_oatpp.sh`, external
      build definitions, compatibility workarounds, and packaging resources.
- [ ] Preserve `/swagger/ui` as a custom, browseable API explorer backed by a
      framework-neutral endpoint catalog (routes, parameters, examples, and
      response codes), without requiring OpenAPI or oatpp-swagger.
- [ ] Run the full test and package matrix.

**Exit criterion:** Creature Server builds, tests, packages, and runs without any
oat++ dependency.

## First implementation slice

Start with `Track`, `AnimationMetadata`, and `Animation` because the existing
round-trip tests cover difficult behavior:

- mutually exclusive `creature_id` and `fixture_id`;
- optional metadata;
- nested arrays and base64 frame strings;
- historical explicit-null behavior;
- mixed creature and fixture tracks;
- GET-to-POST round trips.

The first implementation issue should:

- [x] add the codec conventions and checked-extraction foundation needed by the slice;
- [x] implement and test neutral Track and Animation codecs;
- [x] audit the existing hand-written `animationToJson` against the oat++ response;
- [x] move the corresponding oat++ DTOs to temporary legacy adapters;
- [x] make the affected model headers oat++-free;
- [x] keep all current endpoints and tests operational.

## Testing strategy

### Contract tests

For every public shape, test exact JSON keys and values. Avoid only comparing
`dump()` strings; object key ordering is not part of JSON semantics.

### Negative parsing tests

Each parser needs targeted cases for:

- absent required fields;
- explicit null;
- incorrect primitive and container types;
- signed/unsigned and width overflow;
- empty identifiers and malformed UUIDs;
- duplicate logical identifiers where relevant;
- oversized strings and arrays;
- unknown fields;
- mutually exclusive or conditionally required fields.

### Differential tests during transition

Where current behavior must be preserved, serialize through both the legacy
oat++ path and the new neutral path and compare parsed JSON values. Maintain an
explicit allowlist for intentional differences; do not normalize all differences
away in a generic test helper.

### Integration tests

Keep coverage for behaviors below the DTO layer:

- unread request-body draining and keep-alive framing;
- error envelope and HTTP status mapping;
- HEAD and range responses;
- WebSocket fragmentation, size caps, ping/pong, and broadcasts;
- traceparent propagation and error-span status;
- clean startup and shutdown.

## Safety and architectural invariants

- The 1 ms creature event loop interval must not change.
- JSON parsing and serialization must not move unbounded work onto the event loop.
- All arrays and bodies originating from clients remain bounded.
- Exceptions from JSON conversion must not escape on the event-loop thread.
- Existing OTel span hierarchy and error recording must survive controller changes.
- Creature configuration remains controller-owned; universe assignment remains
  ephemeral. Fixture configuration and universe assignment remain persisted.
- Track validation continues to require exactly one of `creature_id` and
  `fixture_id`.
- The local/travel audio ownership model is unaffected.

## Issue and PR workflow

This document is the architecture and sequencing record. The master GitHub issue
is the operational log.

- Create a child issue for each independently reviewable vertical slice.
- Link child issues and PRs from the matching master checklist item.
- Record intentional wire changes and their Swift-client counterparts in the
  master issue before merge.
- Update this document only when the architecture, invariants, or phase ordering
  changes; use issue comments for chronological progress notes.
- Every behavioral PR receives the required adversarial security and OTel reviews.
- Pure plan/documentation changes do not require those reviews under `AGENTS.md`.

## Definition of complete

The migration is complete when:

1. domain, API, service, messaging, and public voice-library layers contain no
   oat++ types or macros;
2. all JSON contracts have explicit codecs and validation tests;
3. all temporary oat++ DTO adapters are deleted;
4. the selected transport passes HTTP/WebSocket compatibility and lifecycle tests;
5. macOS and Debian AMD64/ARM64 builds, tests, and packages succeed;
6. oat++, oatpp-websocket, oatpp-swagger, their build scripts, and packaged
   resources are removed from the repository;
7. the Swift client is updated for every approved wire-contract change.
