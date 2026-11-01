# HTTP and WebSocket transport contract

This document defines the externally observable behavior that must survive the
oat++ transport replacement tracked by issue #162. The executable oracle is
`tests/transport/transport_contract_test.py`; it speaks HTTP and WebSocket
directly and has no dependency on oat++, Drogon, Crow, or application internals.

## Running the contract

Run the server against a disposable local database, then execute:

```bash
python3 tests/transport/transport_contract_test.py \
  --base-url http://127.0.0.1:8000
```

The default suite is read-only except for `POST /api/v1/fixture/validate`, which
validates without saving. It refuses non-loopback targets unless
`--allow-remote` is explicit.

Fixture CRUD is opt-in because it writes to MongoDB. The test uses a fresh UUID
and schedules cleanup even when an assertion fails:

```bash
python3 tests/transport/transport_contract_test.py \
  --base-url http://127.0.0.1:8000 \
  --include-mutating
```

Stored-file behavior depends on the server's configured sound library:

```bash
python3 tests/transport/transport_contract_test.py \
  --base-url http://127.0.0.1:8000 \
  --sound-file example.wav
```

`--require-byte-ranges` additionally requires `Range: bytes=0-0` to produce a
`206` response. The oat++ baseline does not currently implement byte ranges, so
this flag is for candidate transports and the intentional compatibility change
that follows framework selection.

`--require-fragmented-message-limit` requires the 64 KiB WebSocket limit to be
applied to the aggregate logical message across continuation frames. The oat++
baseline does not enforce that aggregate limit; candidate transports must pass
this stricter mode before selection.

CTest runs the protocol harness's self-tests without needing a live server. It
does not silently start a server, select a MongoDB database, or mutate data.

## Baseline requirements

### HTTP lifecycle and framing

- The listener accepts HTTP/1.1 on `SERVER_PORT` and defaults to port 8000.
- Persistent connections remain usable after a GET or DELETE carrying an
  unexpected fixed-length or chunked body. The transport consumes or rejects
  the body without allowing its bytes to corrupt the next request.
- Request bodies are bounded while bytes are read, including chunked bodies.
  A body exceeding an endpoint's limit returns `413`; the server must not first
  buffer the entire body.
- Clean shutdown stops accepting requests, ends WebSocket worker loops, and
  joins transport-owned threads. It must not alter the independent 1 ms event
  loop interval.

### JSON responses and errors

- JSON responses use `application/json; charset=utf-8` and contain valid UTF-8.
- Status envelopes have exactly the stable application fields below; an
  endpoint may add `session_id` when its contract calls for one:

  ```json
  {"status":"ok|error|not_found","code":200,"message":"..."}
  ```

- `status` is `ok` for 2xx, `not_found` for 404, and `error` otherwise.
- Invalid path UUIDs are rejected before database access.
- Application-level validation endpoints may deliberately return HTTP 200 with
  a typed validation result. `POST /api/v1/fixture/validate` is one such route:
  malformed fixture JSON produces `valid: false` and populated
  `error_messages`, not an HTTP error envelope.
- Malformed bodies for ordinary request endpoints are client errors, not 500s.

### GET and HEAD

- Every GET route has a HEAD counterpart except WebSocket upgrade routes.
- HEAD returns the GET status and representation headers, including the GET
  body's `Content-Length`, while sending no body.
- Stored files are streamed with a known length rather than copied wholesale
  into an in-memory response.
- Byte-range support is a recorded baseline gap. Candidate spikes must
  demonstrate RFC-style `206` and `Content-Range` behavior before selection;
  enabling it in production is an intentional contract improvement, not an
  accidental framework difference.

### WebSockets

- `GET /api/v1/websocket` performs an RFC 6455 upgrade and returns the correct
  `Sec-WebSocket-Accept` value.
- Client frames are masked; server frames are unmasked.
- Fragmented text messages are accumulated as one logical JSON message.
- Inbound logical messages must be capped at 64 KiB across continuation frames.
  Oversized input is discarded without unbounded allocation and the connection
  is closed. This is a recorded oat++ baseline gap and a candidate requirement.
- Malformed JSON is isolated to that message. The server returns a `notice`
  envelope whose message is `Dropped malformed WebSocket message.` and keeps
  the connection usable.
- Ping payloads are echoed in pong frames. The server also sends periodic pings
  and accepts pongs.
- Outbound application messages can be queued from non-transport threads and
  broadcast to every connected client.
- Shutdown wakes the message loop promptly; it must not wait for the normal
  30-second ping interval.

### Observability

- Each REST endpoint creates a request span before application work and accepts
  W3C `traceparent` context.
- Request spans retain the existing HTTP, endpoint, controller, response-size,
  status, and structured error attributes.
- Service and database spans remain children of the request span.
- High-frequency inbound WebSocket messages continue using `SamplingSpan` at
  the 0.0005 normal-message rate, with errors always exported.
- Asynchronous work retains correlation through parent context, span links, or
  `trigger.trace_id` and `trigger.span_id` attributes.

The black-box suite verifies that a valid `traceparent` is accepted. Parentage
and exported span attributes require the existing telemetry integration tests
or a test collector; HTTP has no response header that can prove server-side
span hierarchy.

## Spike comparison profile

Both candidate spikes must run the same contract suite and separately report:

- macOS and Debian AMD64/ARM64 build results;
- dependency, compile-time, binary-size, and package-size impact;
- idle RSS and request latency;
- request-body limit and keep-alive behavior;
- WebSocket fragmentation, slow-client, cross-thread broadcast, and shutdown
  behavior;
- OTel propagation and span hierarchy;
- any intentional contract differences.

Framework-specific conveniences are not application contracts. Domain models,
API structs, services, messaging business logic, and the event loop must remain
framework-neutral.
