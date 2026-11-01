# Issue #162 transport selection

## Decision

Creature Server will productionize **uWebSockets 20.79.0** as the replacement
HTTP/WebSocket transport for oat++. The comparison phase is complete; the
production migration will be planned separately.

The production migration is specified in
[`uwebsockets-productionization-plan.md`](uwebsockets-productionization-plan.md).
Implementation is proceeding incrementally with uWebSockets as the startup
default for deployable test builds. `--http-transport oatpp` or
`HTTP_TRANSPORT=oatpp` selects the compiled rollback transport without changing
the package or database.

This decision assumes Creature Server's actual threat and deployment model:
the process controls animatronics on a trusted LAN, has no external
authentication proxy, and keeps its timing-critical 1 ms event loop separate
from HTTP work. Robust handling of buggy or malicious LAN clients still
matters, as do bounded memory, prompt hardware shutdown, static Linux builds,
small artifacts, and observable failures. Byte-range delivery was dropped from
the selection requirements because the current application does not need it.

## Candidates

| Candidate | Result | Decision |
| --- | --- | --- |
| uWebSockets 20.79.0 | Passed 12/13 strengthened cases, all 8 concurrency gates, the real Mongo/service/OTel gate, macOS runtime checks, and Debian AMD64 compilation. Final stripped concurrency binaries were 395,936 bytes on macOS ARM64 and 408,992 bytes on Linux AMD64. | Selected. It combines the smallest measured production-shaped adapter, bounded asynchronous I/O, active upstream maintenance, static composition, and no application JSON replacement or source patch. |
| Boost.Beast 1.92.0 | Passed 13/13 without patches and had the strongest protocol-boundary behavior. Its stripped macOS binary was 2.1 MiB, the extracted Boost source was 1.1 GiB, and the representative adapter was 604 lines. | Rejected. The dependency, build, binary, and implementation costs were disproportionate for this LAN service. |
| cpp-httplib 0.54.1 | Passed 13/13 with an explicit reject-and-close policy for GET/HEAD bodies. Stripped binaries were 948,744 bytes on macOS and 922,328 bytes on Linux. | Rejected. Its blocking worker model and private buffered-read behavior made concurrency and connection lifecycle less attractive than uWebSockets, even with plentiful CPU cores. |
| RESTinio 0.7.9.1 | Passed 11/13. Stripped binaries were 788,088 bytes on macOS and 725,528 bytes on Linux. | Rejected. Canonical early chunked-body rejection and pre-allocation WebSocket frame limits could not both be enforced at the adapter boundary. |
| Crow 1.3.3 | Passed 11/13 without patches. | Rejected. It accumulates unterminated chunked bodies and enforces its WebSocket limit per frame rather than per fragmented logical message. |
| Drogon 1.9.13 | Implemented the representative slice. | Rejected. The spike required a framework source patch for aggregate WebSocket limits and manual commit pins for core dependencies, which is not an acceptable maintenance model here. |

## Why uWebSockets wins despite one failed case

uWebSockets delays rejection of one malformed frame: an unmasked client frame
declaring a `UINT64_MAX` payload is held while the parser waits for four bytes
it treats as a mask key, then closes at the idle timeout. The application
handler cannot intercept that parser state without an upstream change, source
patch, or lower-level filter.

The gate held 128 such connections simultaneously and demonstrated responsive
health checks and shutdown. Each attack consumes one ordinary asynchronous
connection; it does not allocate the declared payload or occupy a blocking
worker. Under the LAN threat model, this limitation is accepted for selection,
provided productionization adds global and per-peer connection admission.

The alternatives that achieved 13/13 imposed larger ongoing costs. Beast was
substantially larger in source, binary, and adapter complexity. cpp-httplib was
smaller than Beast but retained a blocking request model and a connection
buffering constraint. Those are more important to the long-lived production
architecture than uWebSockets' isolated delayed-close behavior.

## Evidence preserved by this checkpoint

The committed uWebSockets spike contains:

- the shared black-box HTTP/WebSocket contract and strengthened malformed-input
  cases;
- bounded application and file executors with loop-only uWebSockets object
  access;
- streamed dialog-audio delivery, slow-client and FIFO safety checks;
- queue saturation, disconnect, backpressure, broadcast, parser-stall, and
  shutdown concurrency gates;
- a read-only integration target using the production fixture service, MongoDB
  implementation, and `ObservabilityManager`; and
- a local collector test that validates W3C propagation, request/service/DB
  hierarchy, error-first WebSocket sampling, and linked async broadcast work.

Both required checkpoint reviews were completed. They found no remaining
critical issue in the spike. The Mongo driver lifetime and cross-thread request
span mutation findings were fixed and re-reviewed successfully.

## Productionization gates

Selection does not make the spike a production transport. The implementation
plan must cover at least:

1. Mongo socket/query deadlines and bounded shutdown behavior.
2. A bounded production application executor and loop-owned request lifecycle.
3. Per-logical-message WebSocket processing, ordering, and sampled tracing.
4. Common request instrumentation for every REST and file-transfer route.
5. Global and per-peer connection admission plus accept-error backoff.
6. Parser rejection, backpressure, queue, and shutdown metrics.
7. Full production controller/service routing without test-only endpoints.
8. The native macOS and Debian AMD64/ARM64 build, test, and package matrix.
9. Verification that transport load does not disturb the independent 1 ms
   creature event loop.
10. Removal of oat++, oatpp-websocket, Swagger resources, and their build
    infrastructure only after behavioral parity is demonstrated.

## Fresh-session handoff

Start from branch `codex/uwebsockets-transport-spike`. The spike is intentionally
kept separate from `main`; do not reuse the old candidate worktrees as a
production implementation base.

Read these files first:

- `docs/transport-selection-issue-162.md` — decision, evidence, and remaining
  production gates;
- `spikes/uwebsockets-transport/README.md` — detailed uWebSockets behavior and
  accepted limitation;
- `spikes/uwebsockets-transport/main.cpp` — bounded executor, loop dispatcher,
  file streaming, WebSocket, shutdown, and gate instrumentation prototypes;
- `tests/transport/transport_contract_test.py` — shared black-box protocol
  contract;
- `tests/transport/uwebsockets_concurrency_gate_test.py` — concurrency and
  lifecycle gates; and
- `tests/transport/uwebsockets_otel_mongo_gate_test.py` — real production
  service/Mongo/OTLP hierarchy gate.

The concurrency tests expect a current dialog asset named `dialog-render.wav`.
For local reproduction, copy one from `/Volumes/creatures/sounds/dialog` into a
disposable sounds directory. Do not commit the audio asset.

Standalone build and concurrency gate:

```bash
cmake -S spikes/uwebsockets-transport -B build-uwebsockets -G Ninja \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-uwebsockets -j 8
python3 tests/transport/uwebsockets_concurrency_gate_test.py \
  --server build-uwebsockets/creature-server-uwebsockets-spike \
  --sounds-location /path/to/disposable/sounds
```

The broad contract requires starting the standalone server separately. Omit
the byte-range flag; the final malformed unmasked-frame assertion is the one
accepted 12/13 miss:

```bash
SERVER_PORT=18082 build-uwebsockets/creature-server-uwebsockets-spike \
  --sounds-location /path/to/disposable/sounds
python3 tests/transport/transport_contract_test.py \
  --base-url http://127.0.0.1:18082 \
  --include-mutating \
  --sound-file dialog-render.wav \
  --require-fragmented-message-limit
```

The collector-backed integration gate requires Docker and uses loopback ports
4318, 18088, and 27029:

```bash
./build_oatpp.sh
cmake -S . -B build-otel-gate -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCREATURE_BUILD_UWS_OTEL_GATE=ON
cmake --build build-otel-gate \
  --target creature-server-uwebsockets-otel-gate creature-server -j 8
python3 tests/transport/uwebsockets_otel_mongo_gate_test.py
```

The next session should produce a productionization plan, not begin replacing
controllers opportunistically. Resolve ownership, route migration order,
executor/shutdown semantics, WebSocket message ordering, observability parity,
connection admission, rollout, and oat++ removal sequencing before code moves
into the production server.
