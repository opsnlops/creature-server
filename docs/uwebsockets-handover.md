# uWebSockets migration handover

## Purpose and current branch

This is the in-progress replacement of the oat++ HTTP/WebSocket transport with
uWebSockets. Every frozen route is now registered on uWS, but oat++ remains
available as a command-line rollback transport until the differential gates
have been widened and a packaged build has been exercised end to end.

- Branch: `codex/uwebsockets-transport-spike`
- Baseline merge: `f26c8bc` (merged `origin/main` at `162cd14`)
- Target version in the worktree: `3.49.0` (`VERSION.txt`, uncommitted)
- Existing PR: <https://github.com/opsnlops/creature-server/pull/185>
- Tracking issue: <https://github.com/opsnlops/creature-server/issues/211>
- Current registration coverage: **125/125** frozen oat++ routes.

## Do not lose these design decisions

The uWS event-loop thread must not perform database access, filesystem I/O,
speech generation, audio rendering, or event-loop application work. It only
owns uWS sockets and response writes. `UWebSocketsServer.cpp` sends application
work through a bounded `ApplicationExecutor` (4 workers, queue limit 8) and
posts completed `PreparedResponse` values back through `LoopDispatcher`.

Request bodies are collected incrementally with an advertised and actual size
limit. `RequestRegistry` owns response cancellation, disconnect handling, and
shutdown cancellation. Keep new routes on `runBodyRoute` or
`runBodylessRoute`; do not capture/use a `uWS::HttpResponse` in a worker.

### File responses

`PreparedResponse` carries an optional `FilePayload` (path + size). A handler
that wants to serve a file resolves, validates, and sizes it on the application
worker and returns `PreparedResponse::fileStream(...)`; it never reads the
bytes. The loop hands the response to `FileStreamer` (in
`UWebSocketsServer.cpp`), which owns its own small bounded executor (2 workers,
queue 64). Each 256 KiB chunk is read on that executor and posted back to the
loop, which writes it with `tryEnd` and waits on `onWritable` under
backpressure. The request stays in `RequestRegistry` until the last byte is
accepted, so a client disconnect cancels it like any other response; a
mid-stream read failure closes the socket (headers are already on the wire).
HEAD on a file route answers with the file's Content-Length and no body.
Encoded renditions (MP3/Ogg of mono audio, a few MB) are still produced in
memory as oat++ did; only source WAVs stream.

WebSocket `/api/v1/websocket` is migrated. It has per-connection FIFO
mailboxes, bounded inbound/mailbox/backpressure limits, a shared executor,
traceparent-derived per-message metadata, malformed-message notices, and a
broadcast bridge. Do not move message processing back to the loop.

### Tracing across both transports

`creatures::SpanParent` (`util/ObservabilityManager.h`) is a value type that
holds either an oat++ `RequestSpan` or a uWS `OperationSpan`. Every service,
session, and job API that used to take `std::shared_ptr<RequestSpan>` now takes
`SpanParent`, so both transports pass their own span without a cast and the
`*FromOperation` / `parentOperationSpan` duplicates are no longer needed for
new code. `ObservabilityManager` has `createOperationSpan`,
`createChildOperationSpan`, and `createLinkedOperationSpan` overloads for it.
uWS handlers must pass their `span` into services; never `nullptr`.

### Shared handler helpers

`transport/HandlerSupport.{h,cpp}` provides `errorStatus`, `serverErrorStatus`
(oat++-style `error.type` from the ServerError code), `okStatus`, `childSpan`,
`sanitizeSoundFilename`, and `parseBody` (depth-bounded JSON parse plus
contract parse under a `transport.parse.<contract>` child span). New handlers
use it. Older handler files (Animation, CreatureRead, DialogScript,
DialogStream, Document, FixtureRead/Write, Media, Operational, Playlist) still
carry local copies of `failure`/`status`/child-span helpers; collapsing them
onto HandlerSupport is a follow-up.

The uWS default and oat++ rollback switch are already present:

```sh
creature-server --http-transport uwebsockets
creature-server --http-transport oatpp
```

`/api/docs` is the required Swagger-like API browser. `/api/openapi.json`
supplies its data from the frozen manifest.

## Build and validation

The normal `build/` tree contains stale local SDK state. Use this clean,
out-of-tree Debug build instead:

```sh
cmake -S . -B /private/tmp/creature-server-clean-build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build /private/tmp/creature-server-clean-build -j8
```

It emits existing Mongo C++ literal-operator warnings and duplicate library
linker warnings, but no errors. The project CMake gate runs the following
checks automatically:

- framework-neutral source boundary;
- frozen oat++ transport route manifest.

Run these after behavior changes (from the repo root):

```sh
/private/tmp/creature-server-clean-build/creature-server-test
python3 tests/transport/uwebsockets_production_gate_test.py \
    --server /private/tmp/creature-server-clean-build/creature-server --network-device lo0
python3 tests/transport/uwebsockets_file_stream_gate_test.py \
    --server /private/tmp/creature-server-clean-build/creature-server --network-device lo0
(cd tests/transport && python3 transport_contract_test.py --self-test)
```

Both live gates are also registered with CTest when
`CREATURE_RUN_UWS_LIVE_GATE` is set. Before a commit, run `git diff --check`
and `clang-format -i` on each modified C++ file.

Note that a few routes call ElevenLabs synchronously (dialog music finetunes,
plan, refine). Keep them out of automated gates: without a key they return a
400 wrapping the upstream 401, which is parity but also a real network call.

## Route families and where they live

| Family | Handler file |
| --- | --- |
| Creature read/write/register/validate/idle | `CreatureReadHandlers.cpp` (+ `CreatureReadServiceAdapter.cpp`) |
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

The precise source of truth is `docs/transport-route-manifest.json`, not this
table. Recompute registration coverage with:

```sh
python3 - <<'PY'
import json, re, pathlib
p = pathlib.Path('src/server/transport/UWebSocketsServer.cpp').read_text()
got = {(m.group(1).upper().replace('DEL', 'DELETE'), re.sub(r':(\w+)', r'{\1}', m.group(2)))
       for m in re.finditer(r'\.(get|post|put|patch|del|head)\("([^"]+)"', p)}
got.add(('GET', '/api/v1/websocket'))
want = {(x['method'], x['path']) for x in json.load(open('docs/transport-route-manifest.json'))['routes']}
print(f'{len(want & got)}/{len(want)}')
for method, path in sorted(want - got): print(method, path)
PY
```

Body limits on the raw-body routes: lip-sync upload uses
`api::MAX_SOUND_UPLOAD_BODY_BYTES` (1 GiB), STT uses the new
`api::MAX_SPEECH_TO_TEXT_BODY_BYTES` (64 MiB; oat++ had no explicit bound).
Neither goes through the JSON parser.

## What changed in this handover window

- Media/file family (13 routes) ported as one unit with the streaming design
  above; `tests/transport/uwebsockets_file_stream_gate_test.py` proves a
  40 MiB WAV byte-exact with HEAD/GET agreement, mid-stream abort, a
  backpressured slow reader with health probes under 250 ms, eight parallel
  streams, and a FIFO in the sounds directory.
- Music library, dialog music, dialog submit, voice accept, preview, lip-sync,
  and STT routes ported. `MusicCandidateMp3.h` moved from `ws/controller` to
  `ws/service` (it has no oat++ dependency) so both transports share it.
- `SpanParent` refactor closed the tracing seam described in the previous
  handover. `SoundService`'s paired overloads collapsed; `CreatureService`'s
  parallel `parentOperationSpan` parameters removed.
- Parity fixes from review: dialog-script validate always reported
  `valid: true`; ad-hoc play mapped multi-universe animations to 400 instead
  of 422; clear-voice skipped the cache entry when the WAV demote failed;
  dialog-script create/update wrapped DB work in a 400 catch; stream handlers
  used a raw nlohmann parse instead of the depth-bounded API parser. Also a
  dangling reference in the oat++ SoundController play route.
- `ApiDocumentation` unit test counts updated to the regenerated manifest
  (106 paths, 125 operations).

The production gate's differential snapshot now also runs forty mutating and
validation cases (one or more per controller) against both transports and
compares status, content type, and the `{code, status, message}` envelope;
the dialog-script validate cases compare the full body. All of them fail
before any database or upstream call, so the oracle is deterministic.

## Suggested next sequence

1. Add a trace-parentage check on a write route to the production gate (the
   current `check_trace_hierarchy` covers one read route).
2. Collapse the older handlers' local helpers onto `HandlerSupport`.
3. Exercise both `--http-transport` values against a real Mongo and the test
   rig, package AMD64, and only then plan removal of the oat++ controllers,
   `ControllerUtils.h`, `HttpResponseHelpers.h`, and the oat++ dependencies.

## Operational cautions

- Mongo calls already have deadline behavior. Do not remove the executor or
  call Mongo on the uWS loop.
- Preserve the one-millisecond application event loop; transport work must not
  affect it.
- All entity/path IDs are UUIDs. Validate before cache/DB/path use.
- Do not deploy or package this incomplete branch without an explicit request.
- Existing server rollback baseline noted by the user was `3.45.12`; no new
  deployment has been performed from this worktree.
