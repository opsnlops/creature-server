# uWebSockets transport notes

uWebSockets is the only HTTP/WebSocket transport. The oat++ implementation
that this replaced was removed in 3.50.0 (issue #212) after 3.49.0 ran in
production with every route on uWS (PR #185, issue #211). This file keeps the
design rules that the code depends on.

## Design rules

The uWS event-loop thread must not perform database access, filesystem I/O,
speech generation, audio rendering, or event-loop application work. It only
owns uWS sockets and response writes. `UWebSocketsServer.cpp` sends
application work through a bounded `ApplicationExecutor` (4 workers, queue
limit 8) and posts completed `PreparedResponse` values back through
`LoopDispatcher`.

Request bodies are collected incrementally under each route's limit; the loop
reserves at most 64 KiB up front regardless of the advertised Content-Length.
`RequestRegistry` owns response cancellation, disconnect handling, and
shutdown cancellation. Keep new routes on `runBodyRoute` or
`runBodylessRoute`; never capture a `uWS::HttpResponse` in a worker.

### File responses

`PreparedResponse` carries an optional `FilePayload` (path + size). A handler
resolves, validates, and sizes a file on the application worker and returns
`PreparedResponse::fileStream(...)`; it never reads the bytes. The loop hands
the response to `FileStreamer`, which owns a small executor whose queue is
sized to the connection limit (one outstanding read per stream). Each 256 KiB
chunk is read into the stream's single buffer and written with `tryEnd` under
`onWritable` backpressure. The request stays registered until the last byte is
accepted, so a disconnect cancels it like any response; a mid-stream read
failure closes the socket. HEAD answers with the Content-Length and no body.
Encoded renditions (MP3/Ogg of mono audio) are produced in memory.

### WebSocket

`/api/v1/websocket` has per-connection FIFO mailboxes, bounded
inbound/mailbox/backpressure limits, the shared executor, traceparent-derived
per-message metadata, malformed-message notices, and a broadcast bridge fed
from the `websocketOutgoingMessages` queue. Do not move message processing
onto the loop.

### Tracing

`creatures::SpanParent` accepts either a `RequestSpan` or an `OperationSpan`.
Service, session, and job APIs take it, and handlers pass their `span`;
never `nullptr`, which produces a detached root trace.

### Shared handler helpers

`transport/HandlerSupport.{h,cpp}` provides `errorStatus`, `serverErrorStatus`,
`okStatus`, `childSpan`, `sanitizeSoundFilename`, and `parseBody`. The older
handler files (Animation, CreatureRead, Document, FixtureRead/Write, Media,
Operational, Playlist) still carry local copies; collapsing them is a
follow-up.

## Route contract

`docs/transport-route-manifest.json` is the frozen public route surface. It is
generated from the registrations in `UWebSocketsServer.cpp` by
`scripts/transport-route-manifest.py --write` and verified by the build with
`--check`. It feeds `/api/openapi.json` and the API browser at `/api/docs`.
HEAD registrations and trailing-slash aliases are not manifest entries.

`tests/transport/transport_contract_expected.json` records the status,
selected headers, and normalized body of 61 requests (reads, validation
failures, and the static pages). It was recorded from a server whose responses
had been verified identical to the retired oat++ transport. When a response
shape changes on purpose, re-record with
`uwebsockets_production_gate_test.py --record` and review that file's diff.

## Build and validation

The normal `build/` tree may contain stale local SDK state. A clean
out-of-tree Debug build:

```sh
cmake -S . -B /private/tmp/creature-server-clean-build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build /private/tmp/creature-server-clean-build -j8
```

Gates, from the repo root:

```sh
/private/tmp/creature-server-clean-build/creature-server-test
python3 tests/transport/uwebsockets_production_gate_test.py \
    --server /private/tmp/creature-server-clean-build/creature-server --network-device lo0
python3 tests/transport/uwebsockets_file_stream_gate_test.py \
    --server /private/tmp/creature-server-clean-build/creature-server --network-device lo0
(cd tests/transport && python3 transport_contract_test.py --self-test)
```

Both live gates register with CTest under `CREATURE_RUN_UWS_LIVE_GATE`, and the
Debian package build runs the production gate against the packaged binary.
The gates launch the server against an unreachable Mongo on purpose; the
executor-saturation check floods MP3 renditions of a generated tone because a
dead-Mongo read short-circuits once the watchdog marks Mongo unpingable.

A few routes call ElevenLabs synchronously (dialog music finetunes, plan,
refine). Keep them out of automated gates.

## Route families

| Family | Handler file |
| --- | --- |
| Creature read/write/register/validate/idle | `CreatureReadHandlers.cpp` |
| Fixture CRUD, universe, pattern, preview, live | `FixtureReadHandlers.cpp`, `FixtureWriteHandlers.cpp` |
| Playlist | `PlaylistHandlers.cpp` |
| Jobs, debug, metrics, static, API browser | `OperationalHandlers.cpp`, `UWebSocketsServer.cpp` |
| Stage and storyboard | `DocumentHandlers.cpp` |
| Animation CRUD/play/interrupt/ad-hoc/lip-sync job | `AnimationHandlers.cpp` |
| Sound list/play, voice list/subscription/create | `MediaHandlers.cpp` |
| Sound files, renditions, provenance, metadata, exchange audio, preview audio, generated music MP3 | `MediaFileHandlers.cpp` |
| Dialog-script CRUD/validate/clear | `DialogScriptHandlers.cpp` |
| Dialog-stream and legacy ad-hoc stream, exchange list/get | `DialogStreamHandlers.cpp` |
| Dialog submit, voice accept, preview lookup/meta/multichannel | `DialogHandlers.cpp` |
| Music library and dialog music JSON | `MusicHandlers.cpp` |
| Lip-sync JSON, lip-sync raw upload, speech-to-text | `UploadHandlers.cpp` |

Body limits on the raw-body routes: lip-sync upload uses
`api::MAX_SOUND_UPLOAD_BODY_BYTES` (1 GiB), STT uses
`api::MAX_SPEECH_TO_TEXT_BODY_BYTES` (64 MiB). Neither goes through the JSON
parser.

## Operational cautions

- Mongo calls have deadline behavior. Do not remove the executor or call
  Mongo on the uWS loop.
- Preserve the one-millisecond application event loop; transport work must not
  affect it.
- All entity/path IDs are UUIDs. Validate before cache/DB/path use.
- The cold sound list (first `GET /api/v1/sound` after a restart) scans and
  parses every file and can take several seconds; it is memoized afterwards.
