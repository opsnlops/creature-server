# µWebSockets transport spike

This standalone issue #162 candidate uses the µWebSockets 20.79.0 release,
its release-owned µSockets composition, and the application's nlohmann/json
3.12.0. TLS and compression are disabled for the local-network slice. It does
not use Homebrew libraries, source patches, or a framework-owned JSON type in
application contracts. Byte-range delivery is intentionally outside the
revised candidate requirements.

## Result

The representative slice builds on macOS and Debian Linux with µSockets in a
static archive. The macOS executable has no Homebrew dynamic dependencies; the
Linux executable links only the standard C/C++ runtime libraries. Both builds
emit `-Wshadow` warnings from upstream headers.

The contract passes all ordinary HTTP/WebSocket behavior without the byte-range
flag. This includes bounded incremental fixed/chunked request bodies,
keep-alive request bodies, in-memory fixture CRUD, a streamed 25 MiB dialog WAV,
WebSocket fragmentation and ping/pong, and the strict 64 KiB aggregate
logical-message limit. A correctly masked frame declaring `UINT64_MAX` payload
bytes is also rejected before its payload arrives. The current strengthened
suite reports 12/13 because the malformed unmasked-frame case below is now a
hard assertion.

The strengthened malformed-frame contract has one failure: an unmasked client
frame that declares `UINT64_MAX` bytes is not rejected when its ten-byte header
arrives. The server waits for four bytes it assumes are a masking key, despite
the clear mask bit, until the socket's idle timeout. The application message
handler cannot intercept this parser state, so fixing it would require an
upstream change, a source patch, or a lower-level protocol filter.

This standards miss is not resource-amplifying like a blocking worker or early
allocation: each malformed client retains one ordinary asynchronous connection.
The concurrency gate holds 128 such connections simultaneously and verifies
that health requests and two-second shutdown remain unaffected. Selecting this
candidate therefore requires explicitly accepting delayed close for this one
malformed header under the project's LAN threat model.

## Production-shaped concurrency gate

The adapter now uses a bounded four-thread executor with eight queued slots for
JSON parsing and database-shaped fixture work. Only the uWebSockets loop owns
HTTP response and WebSocket objects; worker completions return through
`Loop::defer`. Aborted requests invalidate their response state before a worker
can complete. A dispatcher gate prevents workers from posting after shutdown.

Eight black-box concurrency tests pass on macOS and Debian Linux:

- health remains responsive while the blocking executor is saturated, and
  overload receives a prompt `503` instead of growing an unbounded queue;
- disconnecting while work is in flight is safe;
- worker-originated broadcasts are marshalled back to the transport loop;
- a non-reading WebSocket is closed at the 64 KiB backpressure limit;
- 128 simultaneous connections stalled in the known malformed-frame parser
  state do not delay health checks or shutdown;
- a non-reading client requesting the 25 MiB dialog WAV leaves health checks
  responsive;
- a FIFO accidentally placed in the sounds directory is rejected without
  blocking file workers or shutdown; and
- SIGTERM closes an active WebSocket and cancels a ten-second cooperative task,
  exiting within two seconds.

Sound lookup uses `openat`/`O_NOFOLLOW` against a pinned directory descriptor,
and `pread` runs on a separate bounded two-thread file executor. Only `tryEnd`
and backpressure callbacks run on the uWebSockets loop. The slow-sound test
injects 250 ms into every file read and verifies health remains responsive.
Fixture body collection remains incremental on the transport loop, but parsing,
serialization, and store access occur on the bounded application executor.

Fixed-length bodies advertising more than 1 MiB receive an immediate canonical
`413` plus connection close before allocation or body arrival. The contract
sends only the oversized headers to enforce this behavior.

The concurrency-gate adapter strips to 395,936 bytes on macOS arm64 and 408,992
bytes on Debian amd64. It dynamically links only the platform C/C++ runtimes.

Production integration still needs explicit timeouts for blocking service and
MongoDB calls: cancellation is cooperative, so a task that ignores its stop
token can delay executor teardown. The real 1 ms event loop must also be tested
under the integrated transport; this standalone spike proves thread separation,
not full-process scheduler behavior.

## OTel and real-service integration gate

An opt-in `CREATURE_BUILD_UWS_OTEL_GATE` target links the same uWebSockets
adapter to the production `ObservabilityManager`, static Mongo C/C++ drivers,
`DmxFixtureService`, and fixture database implementation. The read-only fixture
methods live in a small separate production translation unit so the gate does
not need to link the fixture pattern/event-loop subsystem merely to exercise a
real service call. In this target, production-looking fixture mutation and
validation routes are disabled; the in-memory POST/DELETE/validate stand-ins
remain available only in the standalone contract target.

The collector-backed test starts a disposable Mongo 7 container, seeds a real
fixture document, and sends a GET with a known W3C `traceparent`. A local OTLP
HTTP receiver decodes the exported protobuf and asserts this ancestry:

`remote caller → uWS request → DmxFixtureService.getFixture → Database.getFixture → Mongo query`

It also verifies loop-side response completion attributes and status, a traced
WebSocket 101 upgrade, 0.05% (`0.0005`) `SamplingSpan` sessions with forced
export on malformed JSON, and a linked `WebSocket.broadcast` span that ends
only after worker work returns to the uWebSockets loop and calls `publish`.
Broadcasts use an RAII completion guard: cooperative interruption records
`shutdown_cancelled`, failed loop deferral records `defer_failed`, and work
discarded from the queue during shutdown records `abandoned` instead of
silently ending as success.

This is deliberately a positive-path selection gate, not production-complete
instrumentation. Production adoption still needs common request-span coverage
for every route, per-logical-message WebSocket sampling and upgrade correlation,
Mongo socket/query deadlines with bounded shutdown behavior, parser-rejection
telemetry, and global/per-peer connection admission. The gate's Mongo and trace
globals are explicitly released after its workers join so the Mongo driver
instance remains alive through pool teardown.

Run the gate with:

```bash
cmake -S . -B build-otel-gate -G Ninja -DCREATURE_BUILD_UWS_OTEL_GATE=ON
cmake --build build-otel-gate --target creature-server-uwebsockets-otel-gate
python3 tests/transport/uwebsockets_otel_mongo_gate_test.py
```

There is also a release API defect in v20.79.0:
`HttpResponse::maxRemainingBodyLength()` does not compile because its backing
`HttpResponseData` type has no such member. The spike does not patch the
framework; it validates `Content-Length` from the parsed request and uses
`onDataV2`'s remaining-length argument for incremental chunked enforcement.
