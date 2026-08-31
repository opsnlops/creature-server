# Oat++ DTO and service coupling inventory

**Snapshot:** 2026-08-30
**Master issue:** [#162](https://github.com/opsnlops/creature-server/issues/162)
**Related plan:** [oatpp-migration-plan.md](oatpp-migration-plan.md)

## Purpose

This is the Phase 0 inventory for the oat++ migration. It records the complete
DTO definition surface, service signatures that expose oat++ types, important
runtime consumers outside controllers, existing neutral JSON conversion paths,
and the dependency order implied by those relationships.

This document inventories coupling; it does not propose code changes beyond the
sequencing already established in the master plan.

## Baseline

| Category | Classes | Definition files |
|---|---:|---:|
| Domain/model DTOs in `src/model` | 41 | 19 |
| Metrics DTO outside the model/DTO directories | 1 | 1 |
| REST/shared API DTOs in `src/server/ws/dto` | 60 | 34 |
| WebSocket DTOs in `src/server/ws/dto/websocket` | 16 | 11 |
| Inbound WebSocket command DTOs in `src/server/ws/messaging` | 9 | 5 |
| Public `CreatureVoicesLib` model DTOs | 4 | 4 |
| **Total** | **131** | **74** |

The 131 classes declare 479 `DTO_FIELD` entries. There are 176 source, test,
and `CreatureVoicesLib` files containing an `oatpp`/`OATPP` reference; 167 are
in Creature Server's `src` and `tests` trees and nine are in
`CreatureVoicesLib`.

Counting rule: a class is included when it directly derives from `oatpp::DTO`
or derives from the oat++-backed `ListDto`/`WebSocketMessageDto` templates. DTO
descriptions and endpoint-only Swagger annotations are not counted as separate
contracts.

## 3.45.9 Dialog checkpoint

The Dialog controller family now uses neutral request, queued-job, and response
contracts, and seven Dialog DTO adapter files plus the now-unused generic
`ListDto` have been removed. There are 58 files under `src` with a case-insensitive
oatpp reference, down from 67 after the streaming checkpoint. The remaining
references are confined to controller/transport infrastructure, WebSocket
transport and messaging, the 15 surviving DTO adapter files, and two HTTP-client
or compatibility helpers. `src/model`,
`src/api`, the service layer, and `JobWorker.cpp` remain oat++-free.

## 3.45.10 neutral-boundary checkpoint

The service inventory was re-audited before beginning the next slice. Contrary
to the release-status shorthand, `AnimationService` was already fully neutral;
Phase 3 is complete across the entire service directory.

The remaining oat++ component lookups in WebSocket message handlers were only
being used to acquire the application logger. Logger resolution now happens in
`AppComponent`, at the transport boundary, and `MessageProcessor` passes an
ordinary `std::shared_ptr<spdlog::logger>` to its handlers. This removes oat++
from WebSocket messaging business code without changing the 1 ms event loop or
the 0.05% inbound-message sampling strategy.

`cmake/CheckNeutralFrameworkBoundary.cmake` now runs as a required build target
and a CTest. It rejects case-insensitive oat++ references in `src/model`,
`src/api`, every service, `JobWorker`, voice code, and WebSocket messaging. The
`src` reference count at this checkpoint is 51 files, all outside that enforced
boundary.

## 3.45.11 DTO-removal checkpoint

The final six runtime DTOs (`AudioCachePruneDto`, `JobCreatedDto`,
`JobStateDto`, `MakeSoundFileRequestDto`, `SpeechToTextDto`, and `StatusDto`)
now use checked neutral request parsing and explicit JSON response serializers.
The audit also found nine dead adapter files left behind by earlier vertical
slices; those adapters and their DTO-only tests have been deleted. No oat++ DTO
definition remains under `src`.

The only files left in `src/server/ws/dto` are the neutral WebSocket
`MessageTypes` declarations. That directory is now included in the enforced
framework-neutral boundary. The current `src` reference count is 36 files,
confined to controller/transport infrastructure and HTTP compatibility helpers.

## Domain and model DTOs

These are the most important architectural coupling because oat++ declarations
live beside otherwise framework-neutral domain structs.

| File | DTO classes | Current role | Neutral path already present |
|---|---|---|---|
| `src/model/AdHocExchange.h` | — | Domain and persistence values; REST responses use neutral API JSON | Strict, bounded `adHocExchangeFromJson`; persistence and public response serializers are separate |
| `src/model/Animation.h` | — | REST response and accepted POST input use neutral JSON | Strict model-owned `animationToJson` / `animationFromJson`; API and persistence envelopes are distinct |
| `src/model/AnimationMetadata.h` | — | Animation list/detail response and nested input use neutral JSON | Strict model-owned `animationMetadataToJson` / `animationMetadataFromJson` |
| `src/model/DialogScript.h` | — | REST and job responses use canonical neutral JSON | `dialogScriptToJson`; database-owned `dialogScriptFromJson` |
| `src/model/DmxFixture.h` | — | REST responses; config input is parsed from raw JSON | Strict model-owned `dmxFixtureToJson` / `dmxFixtureFromJson`; persistence still uses its legacy parser |
| `src/model/Input.h` | — | Framework-neutral nested input model | Strict model-owned `inputToJson` / `inputFromJson`; obsolete adapter removed |
| `src/model/Notice.h` | — | Framework-neutral inbound and outbound WebSocket notice | Strict model-owned `noticeToJson` / `noticeFromJson`; obsolete adapter removed |
| `src/model/PlaylistItem.h` | — | Framework-neutral nested playlist input/response | Strict model-owned `playlistItemToJson` / `playlistItemFromJson`; obsolete adapter removed |
| `src/model/PlaylistStatus.h` | — | REST and outbound status payloads | Strict model-owned `playlistStatusToJson` / `playlistStatusFromJson` |
| `src/model/Sound.h` | — | Sound lists, ad-hoc summaries, and heavy metadata response | Model-owned `soundToJson`; nested timing/cue serializers preserve the wire shape and omit empty heavy arrays |
| `src/model/Stage.h` | — | Runtime serialization uses raw neutral JSON to preserve console-owned placement and audio keys | `stageToJson`; database-owned `stageFromJson` |
| `src/model/Storyboard.h` | — | Runtime serialization uses raw neutral JSON to preserve console-owned tile action keys | `storyboardToJson`; database-owned `storyboardFromJson` |
| `src/model/StreamFrame.h` | — | Framework-neutral inbound stream command | Strict model-owned `streamFrameToJson` / `streamFrameFromJson`; valid UUID, E1.31 universe, and capped 512-byte decoded DMX payload |
| `src/model/Track.h` | — | Nested animation input/response use neutral JSON | Strict model-owned `trackToJson` / `trackFromJson` |

### Model observations

- Stage and Storyboard no longer define transport DTOs; their endpoints have
  always used neutral JSON so opaque client-owned fields round-trip unchanged.
- The existing domain-to-DTO and hand-written JSON paths are not guaranteed to
  match. `Animation` is the first characterized slice: its canonical neutral
  path omits absent optionals, while one legacy test keeps oat++'s explicit-null
  behavior visible until the HTTP response adapter moves to neutral JSON.
- Most inbound entity parsing is implemented as private/static `Database`
  helpers. Neutralization requires moving codec ownership out of the database
  layer even when the underlying parsing logic can be retained.
- Several DTOs have `convertFromDto` functions even though current REST writes
  already arrive as raw JSON. Usage, not the existence of a reverse converter,
  should decide whether a temporary legacy adapter is needed.

## Metrics response

`SystemCounters::snapshot()` provides the framework-neutral 41-field value
snapshot. Both REST and WebSocket counter responses render it with
`systemCountersSnapshotToJson`; there is no metrics DTO adapter.

The 1 ms counter-send event uses `SystemCountersSnapshot` and a deep
`CreatureRuntimeSnapshot`, then renders neutral JSON through
`WebSocketEnvelope`.

## REST and shared API DTOs

### Entity lists and ad-hoc summaries

| File | DTO classes | Direction |
|---|---|---|
| `api/SoundResponses.h` | `AdHocSoundEntry` | Neutral response |
| `api/JsonResponse.h` | `listResponseToJson` | Neutral list response helper; the unused oat++ `ListDto` adapter has been removed |

Animation and ad-hoc animation lists now render through the neutral model and
`api::listResponseToJson`; their dedicated oat++ DTOs have been removed.

The generic list DTO stores both `count` and `items`. The neutral replacement
should derive `count` from the final item vector when serializing.

### Simple request DTOs

| File | DTO classes | Endpoint/resource family |
|---|---|---|
| `api/DialogContracts.h` | `AcceptVoiceTakeRequest` | Strict bounded dialog take acceptance |
| `api/SoundRequests.h` | `GenerateLipSyncRequest`, `PlaySoundRequest` | Strict bounded lip-sync and playback requests |
| `api/VoiceContracts.h` | `MakeSoundFileRequest` | Strict, bounded voice generation/job input |
| `api/AnimationRequests.h` | `PlayAnimationRequest`, `RegenerateAnimationLipSyncRequest`, `CreateAdHocAnimationRequest`, `TriggerAdHocAnimationRequest` | Strict bounded animation control and ad-hoc speech requests |
| `api/FixtureRequests.h` | `SetFixtureUniverseRequest`, `TriggerFixturePatternRequest` | Strict fixture universe/pattern-trigger requests |

### Structured fixture requests

| File | DTO classes | Direction |
|---|---|---|
| `api/FixtureRequests.h` | `PreviewFixturePatternRequest`, `SetFixtureLiveRequest`, `FixtureChannelValue` | Strict fixture preview/live-control requests |

These request parsers enforce array bounds, channel-name length limits,
byte-range checks, timeout caps, and strict unknown-field rejection.
Sound control requests use the same strict unknown-field/type policy, cap JSON
bodies at 4 KiB, and cap filenames at 255 bytes. Raw WAV uploads use the shared
bounded-body reader with a 1 GiB ceiling.

### Streaming ad-hoc contracts

The stateful start/text/finish contract and exchange responses now use the
framework-neutral, strictly parsed contracts in `api/StreamingAdHocContracts.h`.
Request bodies, text, transcript size, part count, active session count, and
list limits are bounded. Idle sessions expire after ten minutes even when
render work remains queued, with a thirty-minute absolute lease. Each active
session uses one render worker and may retain at most 16 pending sentences;
all sessions share a 128-render reservation ceiling instead of creating a
native thread for every sentence. Background sentence spans are linked to the HTTP
request that launched each asynchronous TTS pipeline, with child spans for
PCM wrapping, lip-sync construction, track construction, and playback. The legacy
`StreamingAdHocDto.h` and `AdHocExchangeDto.*` adapters have been removed.

### Dialog and preview contracts

The Dialog, preview, script, voice-acceptance, and music controllers now parse
bounded raw request bodies into the strict contracts in `api/DialogContracts.h`
and emit neutral JSON. Queued-job detail/result serialization uses those same
contracts, and `DialogScript` responses use `dialogScriptToJson`. The obsolete
`DialogDto`, `DialogMusicDto`, `DialogVoiceDto`, `DialogScriptDto`, preview-export,
and validation DTO adapters have been removed. The five controllers still use
oat++ only for route declaration and byte transport pending the transport phase.

### Validation and generic responses

| File | DTO classes | Direction |
|---|---|---|
| `api/DialogContracts.h` | `DialogScriptValidationResponse` | Neutral response |
| `api/FixtureResponses.h` | `FixtureConfigValidationResponse` | Neutral response |
| `api/JobResponses.h` | `JobCreatedResponse`, `JobStateResponse` | Neutral responses |
| `api/VoiceContracts.h` | `SpeechToTextResponse` | Neutral response |
| `api/JsonResponse.h` | `StatusResponse` | Canonical neutral status response |
| `api/DebugResponses.h` | `AudioCachePruneResponse` | Neutral response |

The controller-level canonical status contract is now framework-neutral:
`api::StatusResponse`, `makeStatusResponse`, and `statusResponseToJson` live in
`src/api/JsonResponse.h`. `HttpResponseHelpers` emits that JSON directly and no
longer constructs an oat++ status DTO. Swagger error declarations now document
their raw JSON response body without importing a DTO adapter.

## WebSocket DTOs

`src/api/WebSocketEnvelope.h` now renders the framework-neutral outbound
`{command, payload}` envelope. Cache invalidation, notice, playlist status,
server log, virtual-status-light, creature-activity, idle-state, counter/runtime,
and job broadcasts use it; all unused outbound oat++ wrappers, including the
generic `WebSocketMessageDto`, have been removed.

### Inbound command DTOs

| File | DTO classes | Handler |
|---|---|---|
| `src/server/sensors/SensorReport.{h,cpp}` | `BoardSensorReport`, `DynamixelSensorReport` | Framework-neutral inbound telemetry codecs for both sensor handlers |

`ClientConnection` now parses each inbound frame once through the capped,
framework-neutral envelope codec, validates its `command` and object `payload`,
and dispatches the retained payload JSON value to every typed handler parser.

## ElevenLabs voice client

The former `CreatureVoicesLib` project has been folded into
`src/server/voice/VoiceClient.{h,cpp}`. Its request, response, subscription, and
voice values are plain structs; the client uses the server's `Result<T>` error
contract and the parent project's curl, fmt, spdlog, and nlohmann/json targets.

## Service interface inventory

All nine service interfaces are now framework-neutral. The original coupling
is retained in the summary column as migration history; `rg 'oatpp|OATPP'
src/server/ws/service` returns no matches.

| Service | Oat++-coupled methods | Coupling summary | Neutral target |
|---|---|---|---|
| `AnimationService` | — | Fully neutral: domain/list values, standard IDs, `Result<T>`, and no HTTP assertions/status construction | Completed |
| `CreatureService` | — | Fully neutral: `Result<T>`, request/response structs, and synchronized runtime snapshots | Completed |
| `DmxFixtureService` | — | Fully neutral: domain values, standard IDs, and checked plain request contracts | Completed |
| `PlaylistService` | — | Fully neutral: domain/list/status values and checked plain request contracts | Completed |
| `SoundService` | — | Fully neutral: domain/response values, standard strings, optionals, and `Result<T>` errors | Completed |
| `MetricsService` | — | Neutral `Result<SystemCountersSnapshot>` return | Completed |
| `VoiceService` | — | In-tree voice structs and neutral API requests inside `Result<T>` | Completed |
| `DialogMusicService` | — | Plain request and result structs | Completed |
| `DialogPreviewService` | — | Plain preview request, turn, cache, and response structs | Completed |

### Hidden service coupling

Several service methods with standard-looking signatures can still hide
framework coupling in their implementations. The completed Sound slice converted
path-resolution and playback failures to `Result<T>`; future slices must continue
to search implementations as well as headers.

## Runtime consumers outside controllers and services

| Consumer | DTO/object-mapper use | Migration implication |
|---|---|---|
| `src/server/jobs/JobWorker.cpp` | Parses and serializes dialog, preview, music, and voice jobs through neutral contracts | Completed with the dialog/voice service slice |
| `src/server/eventloop/events/counter-send.cpp` | Builds a neutral counter/runtime JSON snapshot and enqueues it through `WebSocketEnvelope` | Preserve the 1 ms loop guarantees while retaining the existing wire shape |
| `src/server/metrics/StatusLights.cpp` | Builds and serializes `VirtualStatusLightsMessage` | Convert with outbound WebSocket messages |
| `src/server/metrics/counters.{h,cpp}` | Defines and serializes `SystemCountersSnapshot` | Shared REST/WebSocket counter contract |
| `src/server/logging/CreatureLogSink.h` | Converts `LogItem` and serializes `LogMessage` | Logging cannot depend on the future transport; emit neutral JSON through the existing broadcast seam |
| `src/util/websocketUtils.cpp` | Serializes notice, invalidation, playlist, and job messages | Natural home for the first neutral outbound envelope helper |
| `src/server/ws/websocket/ClientConnection.cpp` | Permissive first-pass parsing and error-notice serialization | Replace after neutral inbound envelope/parser exists |
| `src/server/ws/messaging/*Handler.cpp` | Receive the retained neutral payload and an explicitly injected logger | Completed; no framework or object-mapper dependency remains |

Controllers, `AppComponent`, error handling, request draining, HEAD support, and
WebSocket socket/session classes are expected transport coupling and remain out
of the DTO-neutral definition of done until the later transport phase.

## Existing test coverage

Tests directly exercising oat++ DTO conversion or its object mapper are limited
to:

- `tests/model/DmxFixture_test.cpp`
- `tests/server/ws/CreatureActivityMessage_test.cpp`
- `tests/server/ws/ErrorHandler_test.cpp`
- `tests/server/ws/HttpResponseHelpers_test.cpp`

Existing nlohmann/parser coverage additionally includes AdHocExchange,
Animation (including metadata and Track), Creature, Track dual-ID validation, Storyboard, LogItem, and the fake
database. It is not complete contract coverage: most REST request DTOs, generic
responses, outbound WebSocket messages, sensor commands, job shapes, sound
metadata, playlists, counters, and voice-library DTOs have no exact JSON golden
tests.

## Dependency and migration order

The inventory supports this order:

1. **Codec/error foundation.** Required by every later slice.
2. **Track + AnimationMetadata + Animation.** Existing round-trip tests and
   difficult nullable/mutually-exclusive semantics make this the proof slice.
3. **Generic status and list responses.** The neutral `StatusResponse` and list
   JSON primitives now prevent new oat++ response construction in controllers;
   retire the remaining `StatusDto`/`ListDto` adapters as their service slices
   move to neutral models and codecs.
4. **Leaf outbound WebSocket payloads.** Notice, invalidation, playlist status,
   log item, and status lights can validate the neutral envelope with limited
   service impact.
5. **Creature + counters/runtime snapshots.** Required by the counter event and
   several WebSocket messages.
6. **Fixture, playlist, and sound vertical slices.** Each removes a complete
   service's oat++ surface.
7. **Dialog/jobs.** Completed: controllers, services, jobs, models, and response
   serialization share neutral contracts; only route transport remains oat++.
8. **Inbound WebSocket commands.** Share the neutral envelope but require strict
   payload validation and message-size preservation.
9. **CreatureVoicesLib adapters.** Can overlap VoiceService work once neutral
   request/response conventions are stable.
10. **Delete legacy DTOs and enforce boundaries.** Only after all listed runtime
    consumers are neutral.

## Phase 0 disposition

- Complete DTO inventory grouped by role: **complete**.
- Service signatures containing oat++ types: **complete**.
- Non-web runtime DTO producers/consumers: **complete**.
- Golden JSON contract tests: **in progress** — exact Track/Animation serializer
  shapes are covered; other contract families remain.
- Negative contract tests: **in progress** — Track/Animation now cover missing,
  explicit null, wrong type, UUID/relationship failures, integer overflow,
  unknown fields, collection caps, frame payload caps, and stage geometry.
- Global optional-field classification: **decided** — omit absent optional
  values rather than preserving oat++'s explicit-null wrapper behavior. Detailed
  per-field tests begin with server issue #163 and Creature Console issue #83.

## Track/Animation proof-slice status

The first neutral parsing boundary is implemented without removing the current
transport DTOs yet:

- `Track`, `AnimationMetadata`, and `Animation` parsing is model-owned and
  returns `Result<T>` with field-path `InvalidData` errors.
- API input rejects unknown fields and database-only envelope fields. Persistence
  input permits only `_id` and `created_at` in addition to the canonical shape.
- Validated models are reserialized before MongoDB writes, preventing ignored
  client fields from becoming stored future behavior.
- Optional fields are omitted when absent; explicit null is rejected.
- Animation bodies, track/frame collections, individual encoded frames, and
  total encoded frame data are bounded. The HTTP body limit is enforced while
  bytes are transferred, before a JSON object tree is allocated.
- Limits follow the actual storage and output boundaries: requests are capped
  at 16 MiB, normalized JSON/BSON at 15 MiB, decoded DMX frames at 512 bytes,
  aggregate frame entries at 500,000, and playback at 1,000 ms per frame /
  24 hours total. Frame data is validated as base64 during ingestion rather
  than first failing on the event loop.
- Track IDs and creature/fixture targets must be unique within an animation.
- Render choices and stage placements permit at most one entry per creature.
- API JSON nesting and opaque stage-placement extensions have independent
  depth, field-count, key, string, and byte bounds.
- Stage placement snapshots preserve console-owned extra keys after validating
  the geometry the server consumes.
