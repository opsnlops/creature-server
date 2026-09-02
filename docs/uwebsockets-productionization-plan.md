# uWebSockets productionization plan

**Status:** Accepted; implementation in progress with uWebSockets as the test
deployment default and oat++ retained as a startup-selectable rollback

**Decision:** Productionize uWebSockets 20.79.0 as the HTTP/WebSocket transport

**Decision record:** [`transport-selection-issue-162.md`](transport-selection-issue-162.md)

**Spike evidence:** [`../spikes/uwebsockets-transport/README.md`](../spikes/uwebsockets-transport/README.md)

## Goal

Replace oat++ and oatpp-websocket without changing the public REST/WebSocket
contract, weakening shutdown behavior, or disturbing the independent 1 ms
creature event loop. The migration is complete only when the uWebSockets server
has behavioral and observability parity, has survived the native build/package
matrix and a production canary, and all oat++ build and runtime resources have
been removed.

This is a transport replacement, not an API redesign. Route paths, methods,
status codes, canonical JSON envelopes, file headers, WebSocket envelopes, and
client-visible ordering remain compatible unless a separate change is reviewed
and released explicitly.

## Implementation progress

Landed in the working branch so far:

- the transport lifecycle boundary, startup selector, uWebSockets build, loop
  thread, bounded four-worker/eight-slot application executor, connection
  admission, request tracing, and the first root/health/metrics routes;
- a loop-thread-owned `RequestRegistry` keyed by monotonic request ID and
  generation. Workers now carry only tokens, immutable prepared responses, and
  worker-owned operation spans; they never retain a response pointer or mutate
  the request span;
- explicit queued-task abandonment and shutdown ordering: stop connection
  admission, request executor stop, cancel every registered request, close the
  loop dispatcher, close the app, then join workers before shared services or
  Mongo can be destroyed;
- the frozen 109-route contract manifest and parity checks;
- finite Mongo server-selection, connect, socket, pool-wait, server-operation,
  and write-concern limits, with lower caller-supplied limits preserved;
- operation-scoped Mongo pool leases in place of thread-local leases, plus
  joined watchdog/request/job/event-loop shutdown before pool destruction.
- a dependency-free, same-origin API browser at `/api/docs` (with
  `/api/docs/` accepted as an alias) and its
  manifest-derived OpenAPI 3.1 catalog at `/api/openapi.json`, served by both
  transports with redirect and HEAD behavior; and
- the first two read-only route slices—fixture and creature list/get—implemented
  once as framework-neutral handlers and exposed through thin oat++ and
  uWebSockets adapters. Unit tests lock the list envelopes, UUID rejection,
  not-found mapping, success bodies, and creature runtime-state serialization;
  and
- an opt-in black-box gate that launches the real production executable with
  the default uWebSockets transport and an unreachable loopback Mongo endpoint. It
  verifies API-browser/static behavior, the Mongo deadline, disconnect safety,
  bounded-queue rejection without starving health, and bounded process
  shutdown while four Mongo reads are in flight. Configure it with
  `CREATURE_RUN_UWS_LIVE_GATE=ON` and
  `CREATURE_UWS_LIVE_GATE_NETWORK_DEVICE=lo0` on macOS (`lo` on Linux). The
  same gate boots oat++ afterward and compares status, selected headers, and
  bodies for every currently migrated static, metrics, fixture-read, and
  creature-read route. An embedded OTLP collector also proves that a production
  uWebSockets request preserves its remote parent and nests `http.application`,
  service, and Mongo database spans in order while recording the transport,
  resource ID, database system, terminal outcome, and HTTP status.

The default transport is now uWebSockets for deployable test builds; oat++
remains available through `--http-transport oatpp` or `HTTP_TRANSPORT=oatpp` as
a same-package, restart-time rollback. This is a canary/testing default, not a
declaration that every route family is parity-complete. The live production gate now covers the
oat++/uWebSockets differential, dead Mongo, disconnect, saturation, shutdown,
API-browser delivery, and production trace hierarchy for the migrated read
slices. A second native OS run remains open. The API browser still requires
schema/example enrichment. The Debian ARM64 package gate now extracts the exact
CPack payload, checks runtime linkage and bundled license files, loads the API
browser and OpenAPI catalog without Internet access, and exercises both the
default uWebSockets startup and explicit oat++ rollback. Native macOS ARM64 and
Debian AMD64 package/install coverage remain open.

The ownership primitives have unit coverage for stale completion generations,
client-abort invalidation, shutdown cancellation, bounded-queue saturation, and
queued-task abandonment. Live-socket disconnect and shutdown timing remain part
of the differential transport gate rather than being inferred from these unit
tests.

## Non-negotiable invariants

1. The creature event loop remains a dedicated thread with an unchanged 1 ms
   interval. No transport callback, executor worker, or shutdown join runs on
   it.
2. Only the uWebSockets loop thread may access `uWS::App`, `HttpResponse`, or
   `WebSocket` objects. `Loop::defer` is the only cross-thread entry into that
   ownership domain.
3. Application work and file work use separate bounded executors. Saturation is
   rejected promptly; it never creates an unbounded queue or falls back to work
   on the transport loop.
4. Every request body has an explicit route policy and size limit. The existing
   64 KiB aggregate logical WebSocket message limit remains in force.
5. A disconnected client cannot leave a live response pointer accessible to a
   worker. Late completions become no-ops with an observable terminal outcome.
6. Shutdown stops admission first and joins every possible Mongo user before
   destroying `Database`, its pool, or `mongocxx::instance`.
7. WebSocket messages from one connection are processed in acceptance order.
   A slow message may delay later messages on that connection, but not work for
   other connections or health checks.
8. Error-first tracing is preserved. Normal high-frequency WebSocket messages
   use the existing 0.0005 sampling rate; failures are always exported.
9. TLS and HTTP/WebSocket compression remain disabled for this trusted-LAN
   deployment. Byte-range delivery remains outside the transport contract.
10. The accepted unmasked `UINT64_MAX` WebSocket-header limitation is not
    hidden. Connection admission and idle timeout bound its impact, and its
    regression test remains enabled.
11. The final server includes a human-oriented API browser at `/api/docs`,
    backed by the transport-neutral OpenAPI document at `/api/openapi.json`.
    It is locally packaged and remains usable without Internet access.

## Target architecture and ownership

```text
network
   |
   v
uWebSockets loop thread
   |-- parse route/headers and incrementally enforce body limits
   |-- own live HTTP/WS objects, admission counters, and response spans
   |-- send prepared responses, stream chunks, and publish WS messages
   |
   +---- bounded application executor ---- services ---- Mongo pool
   |              |
   |              +---- immutable completion via Loop::defer
   |
   +---- bounded file executor ----------- openat/pread/pwrite/encode
                  |
                  +---- bounded chunk completion via Loop::defer

independent 1 ms creature event-loop thread
   ^
   +---- existing thread-safe scheduleEvent/message-queue boundaries only
```

### Production components

Introduce a production transport area rather than moving spike code directly
into `main.cpp`:

- `TransportServer`: lifecycle interface used by `main.cpp` (`start`,
  `beginShutdown`, `join`). During rollout it has oat++ and uWebSockets
  implementations selected once at process startup.
- `UWebSocketsServer`: owns the uWS loop thread, app, listen socket, loop
  dispatcher, connection admission, and route registration.
- `BoundedExecutor`: fixed workers, fixed queued-task capacity, non-blocking
  `trySubmit`, cooperative stop, abandoned-task completion, queue metrics, and
  deterministic join.
- `RequestRegistry`: loop-owned request states keyed by a monotonic request ID
  and generation. It is the only place that stores a live `HttpResponse *`.
- `RouteRequest`: immutable method, normalized route, selected headers, path
  parameters, query parameters, peer identity, body bytes/file reference,
  deadline, and trace context copied before leaving the loop.
- `PreparedResponse`: framework-neutral status, headers, and either bounded
  bytes, a file-stream descriptor, or a bodyless response. It never contains a
  uWebSockets object.
- `WebSocketHub`: loop-owned sockets/subscriptions, bounded outbound queue,
  per-connection inbound mailboxes, ordering, and backpressure policy.
- `ConnectionAdmission`: loop-owned global/per-peer counters and rejection
  policy, with no locks on the transport loop.

Do not port oat++ controller classes line for line. Extract their endpoint work
into small framework-neutral handlers returning `PreparedResponse` (or an
equivalent typed result). While both transports exist, the oat++ controller and
the uWebSockets route are thin adapters around the same handler. The existing
neutral-boundary check for `src/api`, models, services, messaging, jobs, and
DTOs remains and expands to cover the new endpoint handlers.

### Request lifecycle

1. The loop admits the connection/request, chooses the registered route, and
   creates the request span.
2. It snapshots every `HttpRequest` string view needed after the callback.
   Nothing borrowed from uWebSockets crosses threads.
3. It installs `onAborted` before accepting body data or dispatching work.
4. It enforces the route's `BodyPolicy` incrementally:
   - `None`: drain unexpected fixed/chunked bodies to preserve keep-alive;
   - `Json(limit)`: reject an oversized `Content-Length` immediately with a
     canonical 413 and connection close, and enforce the same cumulative limit
     for chunked input;
   - `Upload(limit)`: spool through a bounded chunk pipe to the file executor;
   - `StreamingFile`: response-only policy with no request body.
5. The completed immutable request is offered to the appropriate executor. A
   full/stopping queue receives a canonical 503 immediately.
6. A worker parses JSON and calls services. It receives a stop token and an
   absolute deadline, but never a live socket/response object. It creates child
   service/DB spans using captured trace context and produces a response value.
7. Completion is deferred to the loop with request ID/generation. The loop
   sends only if the registry still contains that live generation; otherwise it
   records `client_aborted`, `deadline_expired`, `shutdown_cancelled`,
   `defer_failed`, or `abandoned` and releases the result.
8. The loop records final response attributes/status and ends the request span.

The request span is loop-owned for mutation and completion. Workers may use its
captured parent context to create child spans but must not set request-span
attributes concurrently. This preserves the hierarchy demonstrated by the
spike without depending on cross-thread mutation being safe.

### Executor sizing and overload

Carry the spike's initial shape into the first integrated build: four
application workers/eight queued tasks and two file workers/eight queued tasks.
Make worker and queue counts configurable with validated finite ranges, but do
not allow `0`, an unbounded value, or caller-runs behavior. Tune the defaults
from macOS, AMD64 Linux, and Raspberry Pi 5 measurements before the default
transport flips.

Use separate admission classes so file downloads or a large upload cannot
consume every Mongo/service slot. Report queue depth, active workers, queue wait
time, execution time, accepted/rejected/cancelled totals, and rejection reason.
Health and the smallest static responses stay loop-local and allocation-bounded
so executor saturation cannot hide liveness.

## Mongo deadlines and bounded shutdown

Cooperative `stop_token` cancellation does not interrupt a blocking Mongo C++
driver call. Production readiness therefore requires deadlines below the
transport shutdown budget, not merely cancellation checks around a call.

### Deadline model

- Normalize the configured Mongo URI once and supply finite
  `serverSelectionTimeoutMS`, `connectTimeoutMS`, `socketTimeoutMS`, and
  `waitQueueTimeoutMS` values even when the caller omits them. Explicit URI
  values may lower these limits but may not silently make them infinite.
- Add server-side `max_time` to find/aggregate/command operations where the
  driver supports it, and bounded write concern timeout for writes. Record the
  effective deadline on DB spans.
- Give every HTTP application task an absolute steady-clock deadline. Before
  starting each expensive stage, compute its remaining budget and refuse to
  begin another Mongo/external-I/O operation if it cannot finish inside it.
- Audit other synchronous I/O reachable from request workers (HTTP calls,
  speech services, codecs, filesystem metadata, and process execution). It
  needs a real timeout or must be handed to an existing asynchronous job
  subsystem that returns 202 promptly.
- Treat file streams separately: they can outlive the normal JSON request
  deadline, but every read/write is bounded, backpressured, disconnect-aware,
  and cancelled at shutdown.

The first implementation PR must choose concrete default budgets from measured
current latency and document them in configuration. As a release gate, a dead
or black-holed Mongo server, an exhausted Mongo pool, and an in-flight query
must not prevent transport shutdown from completing within two seconds. Do not
claim this gate until those failure modes are black-box tested on macOS and
Linux.

### Pool and lifetime work

`Database::getCollection` currently keeps a pool entry in `thread_local`
storage. Productionization must replace that implicit lifetime with an explicit
operation lease (pool entry plus collection) or another scoped API so pool
ownership and wait time are visible and bounded. No returned collection may
outlive its lease.

Centralize process lifetime in an `ApplicationRuntime`-style owner. Its
destruction order must be explicit:

1. close transport admission/listener and reject new work;
2. request cancellation and close HTTP/WebSocket connections on the uWS loop;
3. stop accepting executor jobs, abandon queued work with terminal trace
   outcomes, and join transport/application/file workers;
4. shut down and join job, voice, and other workers that can use `Database`;
5. preserve the existing requirement that transport work is gone before the
   1 ms event loop is stopped;
6. reset DB-backed caches and `Database`, destroying the Mongo pool;
7. destroy `mongocxx::instance` only after the pool and all entries are gone;
8. flush and destroy observability after all span-producing workers are joined.

Signal handlers remain async-signal-safe. They only publish stop intent; normal
code performs all logging, loop deferral, joins, and destruction.

## WebSocket behavior

### Per-logical-message ordering

Use uWebSockets' 64 KiB `maxPayloadLength` with compression disabled; the
message callback is the boundary for one fully assembled logical message.
Copy accepted text into a per-connection mailbox containing a monotonically
increasing sequence number. Each mailbox has both a message-count and byte
limit and allows at most one application-executor task in flight.

When message `N` completes, its loop-deferred completion sends any direct
response, records its terminal outcome, and dispatches `N+1`. Messages from
different connections may run concurrently. Binary messages, overflow, unknown
commands, malformed envelopes, and shutdown each have explicit close/drop
semantics and metrics. Add a gate in which message `N` blocks while `N+1`
would mutate the same creature; assert that `N+1` cannot overtake it.

### Outbound ordering and trace context

Replace the global raw-string outgoing queue with a bounded neutral envelope:

```text
payload, enqueue_sequence, source, enqueued_at, trigger_trace_id,
trigger_span_id, completion/outcome handle
```

All producers enqueue through one API. The uWS loop drains in queue order and
is the only publisher/sender. Preserve per-producer FIFO and define global
ordering as the queue's accepted dequeue order; do not promise ordering for
messages racing from independent producers before enqueue.

Each socket retains the 64 KiB maximum backpressure budget and closes when it
would exceed that budget. Check send/publish results and count success,
backpressure, dropped messages, no-subscriber publishes, and slow-client
closes. Queue overflow must be explicit: safety-critical state messages should
either coalesce by documented key or reject their producer; silent dropping is
not allowed.

### Tracing

- Create and finish a request span for the HTTP 101 upgrade, including peer,
  connection admission outcome, route, and `http.status_code=101`.
- Store the upgrade trace/span IDs and a generated connection ID in WebSocket
  user data; do not keep the ended span object alive for the connection.
- Create one `WebSocket.inbound` sampling span per logical message at 0.0005.
  Link it to the upgrade context and add connection ID, message sequence, byte
  size, command, handler, queue wait, processing duration, and outcome.
- Promote malformed JSON/envelopes, unknown commands, handler failures, queue
  overflow, and exceptions to forced export with the normal error envelope.
- Create linked async publish spans from the originating REST/message span.
  End them only after `app.publish`/`send` executes on the loop, with explicit
  `published`, `no_subscribers`, `backpressure`, `defer_failed`,
  `shutdown_cancelled`, or `abandoned` outcome.
- Preserve ping/pong, close code/reason, connection duration, and aggregate
  counters without emitting an unsampled span for every heartbeat.

## Connection admission and accept failures

Request-level rate limiting is too late for the accepted malformed-frame case.
Admission must run at connection open, before parser work, using the public
uWebSockets connection filter/pre-open capability and the peer address from the
socket. Validate with a dedicated gate that a rejected connection never enters
HTTP or WebSocket parsing.

Start with configurable finite limits (provisional defaults: 512 process-wide
and 64 per peer) and tune them against real controller/console behavior. Keep a
small global reserve so a single peer cannot consume the process-wide budget.
Admission bookkeeping is loop-owned and decremented on every close path,
including HTTP-to-WebSocket adoption, parser failure, idle timeout, and forced
shutdown. Test IPv4, IPv6, IPv4-mapped IPv6 normalization, rapid reconnects,
keep-alive, and upgrade so one physical connection is never counted twice.

Emit active, accepted, rejected, and closed totals by reason, plus peer-limit
and global-limit gauges. Peer address belongs on traces/logs but not as an OTel
metric label.

uWebSockets does not provide an application route callback for a failed
kernel-level `accept`. Before rollout, run the server with a deliberately low
`RLIMIT_NOFILE`, exhaust descriptors, and prove that the selected uSockets
composition does not busy-spin and recovers when descriptors become available.
Measure accept-loop CPU and health latency. If the release does not provide
bounded recovery, productionization is blocked until one of these no-patch
solutions passes the full transport suite:

1. an application-owned acceptor with exponential capped backoff that hands
   accepted sockets to uWebSockets through its public adoption API; or
2. a later upstream uWebSockets/uSockets release containing the needed behavior,
   followed by a repeat of the selection, malformed-input, concurrency, binary,
   and packaging gates.

Also set and package an explicit systemd file-descriptor limit, retain an
emergency reserve descriptor for diagnostics/recovery if the owned-acceptor
path is needed, and expose accept errors and current backoff in metrics. A
source patch to uWebSockets/uSockets is not an accepted solution.

Parser telemetry should distinguish application-observed body/message
rejections from transport-internal closes. If uWebSockets cannot expose an
exact internal parser reason, report `connection.closed_before_route` as an
unclassified counter rather than inventing a reason; retain black-box tests for
each known malformed case.

## Route migration order

There are currently 109 explicit controller endpoints, plus generated HEAD
routes, the default 404/error behavior, CORS handling, Swagger resources, and
the WebSocket upgrade. Freeze a machine-readable route manifest before moving
the first family. The frozen baseline lives at
[`transport-route-manifest.json`](transport-route-manifest.json). CI must
compare the oat++ and uWebSockets registrations to that manifest until oat++
is removed.

Implementation may proceed route by route, but a production process selects
exactly one complete transport at startup. Do not split live routes between two
ports against the same mutable runtime, and do not mirror mutating traffic.

| Order | Route families | Reason and exit gate |
| --- | --- | --- |
| 0 | Common transport behavior | Implement canonical JSON errors, unknown-route handling, request body policies, query/path extraction, CORS, W3C context extraction, HEAD registration, executor dispatch, disconnect safety, and shutdown. The representative contract and concurrency gates pass before application routes move. |
| 1 | `/`, `/api/v1/health`, `/api/v1/metric/counters` | Loop-local/read-only baseline. Verify exact status, headers, JSON, GET/HEAD behavior, saturation responsiveness, and request-span parity. |
| 2 | Read-only JSON routes | Start with fixture get/list as the Mongo/OTel gold path, then creature, playlist/status, animation/ad-hoc, dialog-script, stage, storyboard, job, exchange, sound-list/provenance/metadata, and voice read routes. Verify response snapshots, 400/404/500 mapping, service/DB child hierarchy, and dead-Mongo deadlines. |
| 3 | Validation and persistence CRUD without immediate playback | Fixture validation/upsert/delete/universe assignment, creature validation/upsert, playlist/animation persistence, dialog-script, stage, and storyboard CRUD. Run only against disposable Mongo in differential tests. Verify cache invalidation and outbound-message enqueue, but do not yet enable uWS in production. |
| 4 | File GET/HEAD and upload bodies | Port shared secure path resolution and bounded file streaming first: sound variants, generated/preview/shareable music, and streaming-exchange audio. Then implement the 1 GiB lip-sync upload as a streamed temp-file path with bounded in-memory chunks; never buffer it on the loop or application executor. Verify MIME/cache/content-length/HEAD parity, symlink/FIFO rejection, slow readers, disconnects, and temp-file cleanup. |
| 5 | `/api/v1/websocket` | Install the production hub, connection admission, per-connection serialized inbound processing, bounded outbound queue, automatic ping/pong, backpressure close, per-message tracing, and linked publishes. Pass all ordering, malformed-frame, slow-client, and shutdown gates before routes that depend on progress broadcasts migrate. |
| 6 | Runtime and hardware-affecting controls | Creature registration/idle, fixture pattern/preview/live, animation play/interrupt/ad-hoc/lip-sync, playlist start/stop, and sound play. Verify one request causes one state transition/event, trace IDs cross the scheduling boundary, failures cannot throw through the uWS loop, and event-loop timing is unchanged. |
| 7 | Long-running/job-producing APIs | Dialog, dialog music/preview/voice, voice generation, speech-to-text, streaming ad-hoc session routes, stage rerenders, and other endpoints that return 202/progress. Ensure the request executor only validates/enqueues; long work remains in its bounded owner and shutdown has a terminal job outcome. |
| 8 | Debug routes and API browser | Port debug/cache invalidation last. Replace oatpp-swagger generation with a checked-in/generated OpenAPI artifact derived from the route manifest. Serve a pinned, transport-neutral Swagger-like browser at `/api/docs` and the document it consumes at `/api/openapi.json`. The browser assets must be packaged locally rather than loaded from a CDN. Do not package oatpp Swagger assets. |

For each family, add table-driven black-box cases for every route: success,
missing/invalid path values, malformed/wrong-type/oversized body, service error,
unexpected GET/DELETE body followed by keep-alive reuse, HEAD where applicable,
disconnect during work, executor saturation, and expected trace attributes.
Only then remove the corresponding application logic from the oat++ controller;
the thin oat++ adapter remains until rollout is complete.

### API browser deliverable

The replacement for oatpp-swagger is part of transport parity even though no
generated OpenAPI client depends on it:

- `GET /api/docs` serves a searchable, Swagger-like browser with endpoint
  descriptions, parameters, request/response schemas, examples, and same-origin
  interactive requests. `/api/docs/` is accepted as a compatibility alias; the
  page uses absolute same-origin asset/catalog paths and needs no redirect.
- `GET /api/openapi.json` serves the exact OpenAPI document displayed by the
  browser. It includes the server version and every route in the route manifest.
- Browser JavaScript/CSS/fonts and the OpenAPI document are local package
  artifacts with pinned versions and license notices. The page must not require
  a CDN, telemetry service, or other Internet access.
- The uWebSockets server streams these files through the same bounded static-
  file path and supplies explicit content types, cache policy, HEAD behavior,
  and request instrumentation. The HTML shell may be cached briefly; the
  OpenAPI document must revalidate so it cannot silently describe an older
  server after upgrade.
- OpenAPI describes the HTTP upgrade route and links to the WebSocket envelope
  documentation; it does not pretend that OpenAPI fully models the ongoing
  bidirectional message protocol.
- CI compares documented method/path pairs with the production route manifest,
  validates the OpenAPI document, loads the browser in an offline package
  smoke test, and fails on external asset URLs or a missing route.

The concrete browser library may be pinned Swagger UI, Scalar, or an equivalent
static OpenAPI renderer. That dependency choice does not change the stable
Creature Server URLs above and must not reintroduce oat++.

## Observability parity

Build one common request instrumentation wrapper equivalent to today's
`runEndpoint`, and make route registration impossible without it except for an
explicitly reviewed loop-local health specialization.

Every REST/file route must provide:

- one request span with remote W3C parent, normalized `http.route`, method,
  target, protocol version, peer, user agent, request/body sizes, endpoint and
  handler names, transport framework, queue name, and request ID;
- service operation spans and DB child spans with the existing Creature
  subsystem/database standard as the baseline;
- response status, content type/length, codec and UTF-8 replacement attributes;
- `setError`, `error.type`, `error.code`, `error.message`, and exception
  recording where applicable; and
- `setHttpStatus` on every success and error terminal path, including abort,
  overload, deadline, shutdown, parser/body rejection, defer failure, and
  uncaught exception.

Add transport metrics for:

- connections active/accepted/rejected/closed and admission reason;
- HTTP requests active/completed/aborted, status class, and duration;
- application/file queue depth, wait, active work, saturation, cancellation,
  abandonment, and deadline expiration;
- fixed/chunked body rejections and unclassified pre-route parser closes;
- file bytes/read latency, upload bytes/write latency, and slow-client aborts;
- WebSocket connections, messages, mailbox depth, ordering delay,
  malformed/oversized/unknown-command rejection, publish outcomes,
  backpressure, dropped messages, pings/pongs, and close reason; and
- shutdown phase durations and remaining active work.

Use low-cardinality labels for metrics; UUIDs, route targets, peer addresses,
commands, names, sizes, and detailed outcomes belong on traces. Extend the local
collector gate to cover representative success/error/abort/overload/deadline
paths, WebSocket per-message forced export, and async publish links. Add a
mock-observability route-manifest test so every registered endpoint proves that
it created and terminated a request span.

## Event-loop isolation gate

The standalone spike proves thread separation, not scheduler isolation in the
real process. Before transport rollout:

1. record a repeatable oat++ baseline on macOS, AMD64 Linux, and Raspberry Pi 5
   for event-loop frame duration/lateness, missed deadlines, event queue depth,
   CPU, and memory;
2. add measurement without changing the 1 ms scheduling interval or placing
   blocking export/log work in the frame;
3. run the integrated uWS server under simultaneous keep-alive churn,
   connection-limit attack, saturated Mongo workers, WebSocket message load,
   slow file downloads, and a bounded upload while representative animation,
   DMX, and audio events execute;
4. require no regression outside an agreed pre-recorded tolerance and no
   growth in the event queue attributable to transport work; and
5. repeat shutdown during the load, proving no executor join or Mongo wait runs
   on the event-loop thread.

The benchmark/SLO document and raw results are release artifacts. If isolation
fails, reduce/tune transport worker concurrency or OS scheduling placement; do
not change the 1 ms event-loop interval.

## Build, test, and packaging

### Dependency integration

- Pin the exact uWebSockets 20.79.0 release and use its release-owned uSockets
  composition. Keep TLS and zlib disabled as in the spike.
- Move the dependency definition to top-level CMake and expose a small static
  `uSockets` target plus the header-only uWebSockets include target. Do not use
  Homebrew or a system uWebSockets package and do not patch upstream source.
- Scope suppression of upstream `-Wshadow` warnings to third-party targets;
  retain all project warning flags.
- Add uSockets to `deps_only` and the Docker dependency-cache layer. Include
  pinned source metadata and required license notices in source/package
  compliance output.
- Keep `CREATURE_BUILD_UWS_OTEL_GATE` and the standalone spike until the
  production target subsumes every gate; then retire the duplicate spike target
  in a separate cleanup.

### Required matrix

- Native macOS ARM64 Debug and Release build, unit tests, transport contracts,
  concurrency/shutdown gates, and runtime dependency inspection.
- Debian AMD64 and ARM64 Release/RelWithDebInfo builds and tests through the
  package workflow.
- Sanitizer jobs for request disconnect/completion races, mailbox/executor
  shutdown, and connection accounting where supported.
- `.deb` install/upgrade/remove tests, systemd start/stop/restart, configured
  `LimitNOFILE`, health/WebSocket/file smoke tests, and confirmation that the
  binary has only intended dynamic dependencies.
- Package-size and stripped-binary regression reporting; size is informative,
  while behavior, boundedness, and shutdown are release gates.

The uWS-enabled package must contain everything needed at runtime and no build
tree paths. The final oat++-free package must not contain
`/usr/share/creature-server/swagger-ui` from oatpp-swagger. It must contain the
pinned static assets for the replacement API browser, and package-install smoke
tests must load `/api/docs`, fetch `/api/openapi.json`, and verify that the
browser references the packaged document rather than an external URL.

## Rollout and rollback

1. Add `--http-transport=oatpp|uwebsockets` (and one equivalent environment
   setting). Selection is immutable after startup. uWebSockets is the
   deployable-test default; oat++ remains the explicit rollback while routes
   are incomplete.
2. Run the full black-box contract, route-manifest suite, collector gate, and
   failure/concurrency gates against each transport in separate processes.
   Differential mutation tests use disposable Mongo and reset runtime state
   between transports; never mirror a live mutating request.
3. Exercise uWS on a non-hardware CI/lab server, then a controlled rig with real
   controllers and Console clients. Include reconnect, server restart, large
   file, streaming, playlist, and emergency-stop scenarios.
4. Ship a test release containing both transports with uWebSockets default and
   an operator-selectable oat++ rollback. Record admission, queue, latency,
   error, WebSocket, event-loop, memory, and shutdown comparisons.
5. After the canary completes at least one representative full-show cycle and
   all gates stay green, promote the uWebSockets default from test to production
   while retaining oat++ as a restart-time rollback for at least one release.
   No DB schema or API change may be coupled to this promotion.
6. Roll back by selecting oat++ and restarting the process. A rollback must not
   require package downgrade or data repair. Exercise this exact path before
   the default changes.
7. Remove oat++ only after the uWS-default release has completed the agreed soak
   period with no unresolved transport severity-1/2 defects and both required
   security/observability reviews approve the removal diff.

## Final oat++ removal

Perform removal as its own reviewable phase, not mixed into the default flip:

- delete the oat++ `TransportServer`, `App`, `AppComponent`, error/body-drain/
  HEAD adapters, controller classes, WebSocket cafe/connection classes,
  `SwaggerComponent`, and oat++-specific response helpers;
- retain and relocate framework-neutral services, DTOs, message handlers,
  contracts, JSON serializers, and route handlers;
- remove `find_package(oatpp*)`, oat++ include/link entries,
  `OATPP_THREAD_DISTRIBUTED`, Swagger compile definitions/resources, and oat++
  links from both server and test targets;
- remove `build_oatpp.sh`, oat++ external-project declarations/install trees,
  Docker build steps/cache inputs, package Swagger installation, and CI setup;
- remove the runtime transport flag and oat++ fallback after the rollback window;
- update AGENTS.md/build docs and make normal builds no longer require
  `./build_oatpp.sh`;
- replace oatpp Swagger generation with the transport-neutral OpenAPI artifact,
  serve it at `/api/openapi.json`, serve the locally packaged API browser at
  `/api/docs`, and verify the document against the route manifest in CI; and
- require `rg -i 'oatpp'` to find only historical decision/audit documents or
  explicitly allowlisted migration notes. No production source, test target,
  package input, or generated artifact may depend on oat++.

Run the entire native/package matrix, a clean build with no oat++ install tree,
all 109-route parity tests, WebSocket/concurrency/Mongo/shutdown gates, and the
event-loop isolation load before declaring removal complete.

## Phase exit checklist

Promotion from the deployable-test default to the production default may occur
only when all answers are yes:

- [ ] The route manifest, generated HEAD behavior, errors, CORS, files, and
      WebSocket contract match the approved oat++ baseline.
- [ ] All bodies, queues, mailboxes, connections, backpressure, and file chunks
      have explicit finite bounds.
- [ ] Mongo and every synchronous request-time I/O path have tested deadlines.
- [ ] Dead Mongo, pool exhaustion, disconnect, executor saturation, parser
      stalls, slow files, and active WebSockets cannot break the two-second
      transport shutdown gate.
- [ ] Per-connection WebSocket ordering and linked per-message/publish tracing
      pass collector-backed tests.
- [ ] Global/per-peer connection admission occurs before parsing and descriptor
      exhaustion does not busy-spin.
- [ ] Every REST/file route has a request span, every service/DB path has the
      expected hierarchy, and every terminal path records HTTP/error outcome.
- [ ] Integrated transport load produces no unacceptable regression in the
      independent 1 ms event loop on the production hardware classes.
- [ ] macOS ARM64 and Debian AMD64/ARM64 build/test/package/install gates pass.
- [x] `/api/docs` provides the packaged API browser and successfully loads the
      current `/api/openapi.json` without Internet access.
- [x] The startup-selectable oat++ rollback has been exercised from the same
      package.

Oat++ may be removed only when the above remains true with uWebSockets as the
default through the agreed soak, the OpenAPI/route manifest replacement is in
place, and a clean oat++-free build/package passes the same gates.
