# Oat++ DTO and service coupling inventory

**Snapshot:** 2026-08-22
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

## Domain and model DTOs

These are the most important architectural coupling because oat++ declarations
live beside otherwise framework-neutral domain structs.

| File | DTO classes | Current role | Neutral path already present |
|---|---|---|---|
| `src/model/AdHocExchange.h` | `AdHocExchangePartDto`, `AdHocExchangeDto`, `AdHocExchangeListDto` | REST response/list; exchange persistence uses the domain type | `adHocExchangeToJson` and `adHocExchangeFromJson` |
| `src/model/Animation.h` | `AnimationDto` | REST response and accepted POST/ad-hoc input | Strict model-owned `animationToJson` / `animationFromJson`; API and persistence envelopes are distinct |
| `src/model/AnimationMetadata.h` | `CreatureRenderChoiceDto`, `AnimationMetadataDto` | Animation list/detail response and nested input | Strict model-owned `animationMetadataToJson` / `animationMetadataFromJson` |
| `src/model/DialogScript.h` | `DialogScriptTurnDto`, `AcceptedVoiceDto`, `DialogBackgroundMusicDto`, `DialogScriptDto` | Primarily REST and job response; input is already parsed from raw JSON | `dialogScriptToJson`; database-owned `dialogScriptFromJson` |
| `src/model/DmxFixture.h` | — | REST responses; config input is parsed from raw JSON | Strict model-owned `dmxFixtureToJson` / `dmxFixtureFromJson`; persistence still uses its legacy parser |
| `src/server/ws/dto/InputDto.h` (moved from `src/model/Input.h`) | `InputDto` | Temporary oat++ adapter for remaining non-Creature nested uses | Strict model-owned `inputToJson` / `inputFromJson` |
| `src/server/ws/dto/NoticeDto.h` (moved from `src/model/Notice.h`) | `NoticeDto` | Temporary oat++ adapter for inbound and outbound WebSocket notices | Strict model-owned `noticeToJson` / `noticeFromJson` |
| `src/server/ws/dto/PlaylistItemDto.h` (moved from `src/model/PlaylistItem.h`) | `PlaylistItemDto` | Temporary oat++ adapter for nested playlist input/response | Strict model-owned `playlistItemToJson` / `playlistItemFromJson` |
| `src/model/PlaylistStatus.h` | — | REST and outbound status payloads | Strict model-owned `playlistStatusToJson` / `playlistStatusFromJson` |
| `src/model/Sound.h` | — | Sound lists, ad-hoc summaries, and heavy metadata response | Model-owned `soundToJson`; nested timing/cue serializers preserve the wire shape and omit empty heavy arrays |
| `src/model/Stage.h` | — | Runtime serialization uses raw neutral JSON to preserve console-owned placement and audio keys | `stageToJson`; database-owned `stageFromJson` |
| `src/model/Storyboard.h` | — | Runtime serialization uses raw neutral JSON to preserve console-owned tile action keys | `storyboardToJson`; database-owned `storyboardFromJson` |
| `src/model/StreamFrame.h` | — | Framework-neutral inbound stream command | Strict model-owned `streamFrameToJson` / `streamFrameFromJson`; valid UUID, E1.31 universe, and capped 512-byte decoded DMX payload |
| `src/model/Track.h` | `TrackDto` | Nested animation input/response | Strict model-owned `trackToJson` / `trackFromJson` |

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
| `AdHocAnimationDto.h` | `AdHocAnimationDto`, `AdHocAnimationListDto` | Response |
| `api/SoundResponses.h` | `AdHocSoundEntry` | Neutral response |
| `ListDto.h` | `ListDto<T>`, `AnimationsListDto`, `VoiceListDto` | Response |

The generic list DTO stores both `count` and `items`. The neutral replacement
should derive `count` from the final item vector when serializing.

### Simple request DTOs

| File | DTO classes | Endpoint/resource family |
|---|---|---|
| `CreateAdHocAnimationRequestDto.h` | `CreateAdHocAnimationRequestDto` | Ad-hoc animation creation |
| `DialogVoiceDto.h` | `AcceptVoiceRequestDto` | Dialog take acceptance |
| `api/SoundRequests.h` | `GenerateLipSyncRequest`, `PlaySoundRequest` | Strict bounded lip-sync and playback requests |
| `MakeSoundFileRequestDto.h` | `MakeSoundFileRequestDto` | Voice generation/job input |
| `PlayAnimationRequestDto.h` | `PlayAnimationRequestDto` | Stored animation playback |
| `RegenerateLipSyncRequestDto.h` | `RegenerateLipSyncRequestDto` | Lip-sync regeneration |
| `api/FixtureRequests.h` | `SetFixtureUniverseRequest`, `TriggerFixturePatternRequest` | Strict fixture universe/pattern-trigger requests |
| `TriggerAdHocAnimationRequestDto.h` | `TriggerAdHocAnimationRequestDto` | Prepared ad-hoc animation playback |

### Structured fixture requests

| File | DTO classes | Direction |
|---|---|---|
| `api/FixtureRequests.h` | `PreviewFixturePatternRequest`, `SetFixtureLiveRequest`, `FixtureChannelValue` | Strict fixture preview/live-control requests |

These request parsers enforce array bounds, channel-name length limits,
byte-range checks, timeout caps, and strict unknown-field rejection.
Sound control requests use the same strict unknown-field/type policy, cap JSON
bodies at 4 KiB, and cap filenames at 255 bytes. Raw WAV uploads use the shared
bounded-body reader with a 1 GiB ceiling.

### Streaming ad-hoc DTOs

| File | DTO classes | Direction |
|---|---|---|
| `StreamingAdHocDto.h` | `StreamingAdHocStartRequestDto`, `StreamingAdHocTextRequestDto`, `StreamingAdHocFinishRequestDto` | Request |
| `StreamingAdHocDto.h` | `StreamingAdHocStartResponseDto`, `StreamingAdHocTextResponseDto`, `StreamingAdHocFinishResponseDto` | Response |

These form a stateful three-call contract and should migrate as one vertical
slice rather than as six independent DTOs.

### Dialog and preview DTOs

| File | DTO classes | Direction |
|---|---|---|
| `DialogDto.h` | `DialogTurnDto`, `DialogRequestDto`, `DialogPreviewRequestDto`, `DialogPreviewLookupRequestDto` | Request/nested input |
| `DialogDto.h` | `DialogJobResultDto`, `DialogPreviewVoiceSegmentDto`, `DialogPreviewWordTimingDto`, `DialogPreviewCharTimingDto`, `DialogPreviewMetaResponseDto`, `DialogPreviewGenerationEntryDto`, `DialogPreviewLookupResponseDto` | Response/nested output |
| `DialogMusicDto.h` | `DialogMusicRequestDto` | Request and queued-job detail |
| `DialogMusicDto.h` | `DialogMusicGenerationResultDto`, `DialogMusicPromotionResultDto` | Response/job result |
| `DialogPreviewExportResultDto.h` | `DialogPreviewExportResultDto` | Job result |

This is the largest cross-layer DTO family. `DialogPreviewService` accepts DTOs
despite describing itself as HTTP-free, and `JobWorker` parses and emits these
DTOs directly. The dialog slice must include controllers, services, and queued
job detail/result serialization together.

### Validation and generic response DTOs

| File | DTO classes | Direction |
|---|---|---|
| `DialogScriptValidationDto.h` | `DialogScriptValidationDto` | Response |
| `FixtureConfigValidationDto.h` | `FixtureConfigValidationDto` | Response |
| `JobCreatedDto.h` | `JobCreatedDto` | Response |
| `JobStateDto.h` | `JobStateDto` | REST response |
| `SimpleResponseDto.h` | `SimpleResponseDto` | Response |
| `SpeechToTextDto.h` | `SpeechToTextResponseDto` | Response |
| `StatusDto.h` | `StatusDto` | Legacy service-level success/error adapter |

The controller-level canonical status contract is now framework-neutral:
`api::StatusResponse`, `makeStatusResponse`, and `statusResponseToJson` live in
`src/api/JsonResponse.h`. `HttpResponseHelpers` emits that JSON directly and no
longer constructs `StatusDto`. Remaining `StatusDto` references describe legacy
Swagger error responses or belong to resource slices that have not yet migrated.

## WebSocket DTOs

### Outbound and shared messages

| File | DTO classes | Payload/use |
|---|---|---|
| `WebSocketMessageDto.h` | `WebSocketMessageDto<T>` | Legacy generic `{command, payload}` envelope for remaining oat++ messages |

`src/api/WebSocketEnvelope.h` now renders the framework-neutral outbound
`{command, payload}` envelope. Cache invalidation, notice, playlist status,
server log, virtual-status-light, creature-activity, idle-state, counter/runtime,
and job broadcasts use it; all unused outbound oat++ wrappers have been removed.

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

Nine service headers expose oat++ directly.

| Service | Oat++-coupled methods | Coupling summary | Neutral target |
|---|---|---|---|
| `AnimationService` | `listAllAnimations`, `getAnimation`, `getAdHocAnimation`, `upsertAnimation`, `listAdHocAnimations`, `deleteAnimation`, `playStoredAnimation` | DTO/list/status returns, `oatpp::String` IDs, HTTP assertions/status | `Result<T>`, vectors, `Result<void>`, standard/strong IDs |
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
| `src/server/ws/messaging/*Handler.cpp` | Reparse complete messages through oat++ mappers | Convert per inbound command family |

Controllers, `AppComponent`, error handling, request draining, HEAD support, and
WebSocket socket/session classes are expected transport coupling and remain out
of the DTO-neutral definition of done until the later transport phase.

## Existing test coverage

Tests directly exercising oat++ DTO conversion or its object mapper are limited
to:

- `tests/model/AnimationMetadata_test.cpp`
- `tests/model/Animation_roundtrip_test.cpp`
- `tests/model/DialogScript_test.cpp`
- `tests/model/DmxFixture_test.cpp`
- `tests/server/ws/CreatureActivityMessage_test.cpp`
- `tests/server/ws/ErrorHandler_test.cpp`
- `tests/server/ws/HttpResponseHelpers_test.cpp`

Existing nlohmann/parser coverage additionally includes AdHocExchange,
Animation, Creature, Track dual-ID validation, Storyboard, LogItem, and the fake
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
7. **Dialog/jobs.** Largest cross-layer slice; do only after conventions are
   proven elsewhere.
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
