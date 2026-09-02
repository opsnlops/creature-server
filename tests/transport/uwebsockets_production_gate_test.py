#!/usr/bin/env python3
"""Black-box lifecycle gate for the production uWebSockets transport.

This launches the real ``creature-server`` executable in headless RTP mode.
MongoDB deliberately points at an unused loopback port so fixture reads exercise
the production executor, Mongo deadlines, request registry, and shutdown path.
"""

from __future__ import annotations

import argparse
import base64
import concurrent.futures
import hashlib
import http.server
import json
import os
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

import transport_contract_test as contract
import uwebsockets_otel_mongo_gate_test as otel_gate


FIXTURE_ID = "8e3a4b5c-1d2f-4e6a-9b0c-7f8e9d0a1b2c"
MONGO_TIMEOUT_MS = 500
REQUEST_COUNT = 32


class ProductionServer:
    def __init__(self, executable: Path, network_device: str, transport: str | None) -> None:
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0))
            self.port = reservation.getsockname()[1]

        self.sounds = tempfile.TemporaryDirectory(prefix="creature-uws-production-gate-")
        self.output = tempfile.TemporaryFile(mode="w+t", encoding="utf-8")
        environment = os.environ.copy()
        environment["SERVER_PORT"] = str(self.port)
        mongo_uri = (
            "mongodb://127.0.0.1:1/"
            f"?serverSelectionTimeoutMS={MONGO_TIMEOUT_MS}"
            f"&connectTimeoutMS={MONGO_TIMEOUT_MS}"
            f"&socketTimeoutMS={MONGO_TIMEOUT_MS}"
            f"&waitQueueTimeoutMS={MONGO_TIMEOUT_MS}"
            f"&wTimeoutMS={MONGO_TIMEOUT_MS}"
        )
        command = [
                str(executable),
                "--http-max-connections",
                "64",
                "--http-max-connections-per-peer",
                "48",
                "--mongodb-uri",
                mongo_uri,
                "--sounds-location",
                self.sounds.name,
                "--network-device",
                network_device,
                "--rtp-audio",
                "--lip-sync-engine",
                "rhubarb",
            ]
        if transport is not None:
            command[1:1] = ["--http-transport", transport]
        self.process = subprocess.Popen(
            command,
            env=environment,
            stdout=self.output,
            stderr=subprocess.STDOUT,
            text=True,
        )
        self.config = contract.parse_base_url(f"http://127.0.0.1:{self.port}", False)
        self.transport = transport or "default"
        self._wait_until_ready()

    def logs(self) -> str:
        self.output.flush()
        self.output.seek(0)
        return self.output.read()

    def _wait_until_ready(self) -> None:
        deadline = time.monotonic() + 15.0
        last_error: BaseException | None = None
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                raise AssertionError(
                    f"production {self.transport} server exited during startup ({self.process.returncode}):\n"
                    f"{self.logs()}"
                )
            try:
                code, _, _ = contract.http_request(self.config, "GET", "/api/v1/health")
                if code == 200:
                    return
            except (ConnectionError, OSError) as error:
                last_error = error
            time.sleep(0.05)
        raise AssertionError(
            f"production {self.transport} server did not become ready: {last_error}\n{self.logs()}"
        )

    def stop(self, timeout: float = 2.0) -> float:
        if self.process.poll() is not None:
            raise AssertionError(f"production server exited early ({self.process.returncode}):\n{self.logs()}")
        started = time.monotonic()
        self.process.send_signal(signal.SIGTERM)
        try:
            self.process.wait(timeout=timeout)
        except subprocess.TimeoutExpired as error:
            self.process.kill()
            self.process.wait(timeout=2.0)
            raise AssertionError(f"production server did not stop within {timeout:.1f}s:\n{self.logs()}") from error
        elapsed = time.monotonic() - started
        if self.process.returncode != 0:
            raise AssertionError(f"production server exited with {self.process.returncode}:\n{self.logs()}")
        return elapsed

    def close(self) -> None:
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait(timeout=2.0)
        self.output.close()
        self.sounds.cleanup()


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def check_static_contract(server: ProductionServer) -> None:
    code, headers, raw = contract.http_request(server.config, "GET", "/api/v1/health")
    require(code == 200, f"health returned {code}")
    require(json.loads(raw) == {"status": "ok", "code": 200, "message": "Server is operational"},
            "health response changed")
    require(headers.get("content-type") == "application/json; charset=utf-8", "health content type changed")

    code, headers, raw = contract.http_request(server.config, "GET", "/api/docs")
    require(code == 200 and b"Creature Server API" in raw, "API browser was not served")
    require(headers.get("content-type") == "text/html; charset=utf-8", "API browser content type changed")

    code, headers, raw = contract.http_request(server.config, "GET", "/api/docs/")
    require(code == 200 and b"Creature Server API" in raw, "trailing-slash API browser alias was not served")
    require(headers.get("content-type") == "text/html; charset=utf-8", "API browser alias content type changed")

    code, _, raw = contract.http_request(server.config, "GET", "/api/openapi.json")
    catalog = json.loads(raw)
    require(code == 200 and catalog.get("openapi") == "3.1.0", "OpenAPI catalog was not served")
    require("/api/v1/fixture/{fixtureId}" in catalog.get("paths", {}), "fixture route missing from OpenAPI catalog")


def open_websocket(server: ProductionServer) -> tuple[socket.socket, contract.HttpSocketReader]:
    connection = contract.open_socket(server.config)
    try:
        reader = contract.HttpSocketReader(connection)
        websocket_key = base64.b64encode(os.urandom(16)).decode("ascii")
        connection.sendall(
            (
                "GET /api/v1/websocket HTTP/1.1\r\n"
                f"Host: {server.config.host_header}\r\n"
                "Upgrade: websocket\r\n"
                "Connection: Upgrade\r\n"
                f"Sec-WebSocket-Key: {websocket_key}\r\n"
                "Sec-WebSocket-Version: 13\r\n"
                f"traceparent: {otel_gate.TRACEPARENT}\r\n\r\n"
            ).encode("ascii")
        )
        code, headers, body = reader.read_http_response()
        require(code == 101, f"WebSocket upgrade returned {code}: {body[:512]!r}")
        require(body == b"", "WebSocket upgrade returned a body")
        require(headers.get("upgrade", "").lower() == "websocket", "WebSocket upgrade header is missing")
        expected_accept = base64.b64encode(
            hashlib.sha1((websocket_key + contract.WEBSOCKET_GUID).encode("ascii")).digest()
        ).decode("ascii")
        require(headers.get("sec-websocket-accept") == expected_accept, "WebSocket accept hash is invalid")
        return connection, reader
    except BaseException:
        connection.close()
        raise


def read_text_payload(
    connection: socket.socket, reader: contract.HttpSocketReader, expected: bytes, timeout: float
) -> bytes:
    _, _, payload = contract.read_matching_server_frame(
        connection,
        reader,
        lambda final, opcode, candidate: final and opcode == 0x1 and candidate == expected,
        timeout=timeout,
    )
    return payload


def check_websocket_contract(server: ProductionServer, *, require_message_limit: bool) -> None:
    malformed, malformed_reader = open_websocket(server)
    try:
        malformed.sendall(contract.encode_client_frame(0x1, b'{"command":', final=False))
        malformed.sendall(contract.encode_client_frame(0x0, b'"unterminated', final=True))
        _, _, malformed_notice = contract.read_matching_server_frame(
            malformed,
            malformed_reader,
            lambda final, opcode, payload: final
            and opcode == 0x1
            and b"Dropped malformed WebSocket message." in payload,
            timeout=server.config.timeout,
        )
        require(json.loads(malformed_notice).get("command") == "notice", "malformed message notice changed")
    finally:
        malformed.close()

    first, first_reader = open_websocket(server)
    second, second_reader = open_websocket(server)
    try:
        ping_payload = b"production-gate"
        first.sendall(contract.encode_client_frame(0x9, ping_payload))
        contract.read_matching_server_frame(
            first,
            first_reader,
            lambda final, opcode, payload: final and opcode == 0xA and payload == ping_payload,
            timeout=server.config.timeout,
        )

        ordered_messages = [
            json.dumps(
                {"command": "board-sensor-report", "payload": {"gate_sequence": sequence}},
                separators=(",", ":"),
            ).encode()
            for sequence in (1, 2)
        ]
        for message in ordered_messages:
            first.sendall(contract.encode_client_frame(0x1, message))
        for connection, reader in ((first, first_reader), (second, second_reader)):
            for message in ordered_messages:
                read_text_payload(connection, reader, message, server.config.timeout)
    finally:
        first.close()
        second.close()

    if not require_message_limit:
        return

    oversized, oversized_reader = open_websocket(server)
    try:
        for index in range(8):
            oversized.sendall(
                contract.encode_client_frame(0x1 if index == 0 else 0x0, b"x" * (8 * 1024), final=False)
            )
        try:
            oversized.sendall(contract.encode_client_frame(0x0, b"x", final=True))
        except (BrokenPipeError, ConnectionResetError):
            pass
        require(
            contract.wait_for_websocket_close(oversized, oversized_reader, timeout=server.config.timeout),
            "uWebSockets accepted a fragmented message above the 64 KiB limit",
        )
    finally:
        oversized.close()


def check_websocket_trace(capture: otel_gate.OtlpCapture) -> None:
    spans = capture.wait_for_spans({"WebSocket.inbound"}, timeout=12)
    candidates = [
        span
        for span in spans
        if span.get("name") == "WebSocket.inbound"
        and span.get("attributes", {}).get("transport.framework") == "uwebsockets"
    ]
    require(candidates, "uWebSockets inbound message span was not exported")
    attributes = candidates[-1]["attributes"]
    require(attributes.get("websocket.connection.id", 0) > 0, "WebSocket connection ID is missing from tracing")
    require(attributes.get("websocket.message.sequence", 0) > 0, "WebSocket message sequence is missing from tracing")
    require(attributes.get("websocket.command") == "board-sensor-report", "WebSocket command is missing from tracing")
    require(attributes.get("trigger.trace_id") == otel_gate.TRACE_ID, "upgrade trace ID was not linked to the message")
    require(attributes.get("trigger.span_id") == otel_gate.REMOTE_PARENT_ID, "upgrade span ID was not linked to the message")


def differential_snapshot(server: ProductionServer) -> dict[tuple[str, str], tuple[int, dict[str, str], bytes]]:
    cases = (
        ("GET", "/"),
        ("HEAD", "/"),
        ("GET", "/api/v1/health"),
        ("HEAD", "/api/v1/health"),
        ("GET", "/api/docs"),
        ("HEAD", "/api/docs"),
        ("GET", "/api/docs/"),
        ("HEAD", "/api/docs/"),
        ("GET", "/api/openapi.json"),
        ("HEAD", "/api/openapi.json"),
        ("GET", "/api/v1/metric/counters"),
        ("HEAD", "/api/v1/metric/counters"),
        ("GET", "/api/v1/creature"),
        ("HEAD", "/api/v1/creature"),
        ("GET", f"/api/v1/creature/{FIXTURE_ID}"),
        ("GET", "/api/v1/creature/not-a-uuid"),
        ("GET", "/api/v1/fixture"),
        ("HEAD", "/api/v1/fixture"),
        ("GET", f"/api/v1/fixture/{FIXTURE_ID}"),
        ("GET", "/api/v1/fixture/not-a-uuid"),
        ("GET", "/__transport_differential_missing__"),
    )
    compared_headers = ("content-type", "content-length", "location")
    snapshot: dict[tuple[str, str], tuple[int, dict[str, str], bytes]] = {}
    for method, path in cases:
        status, headers, body = contract.http_request(server.config, method, path)
        selected_headers = {name: headers[name] for name in compared_headers if name in headers}
        normalized_body = body
        if path == "/api/v1/metric/counters":
            selected_headers.pop("content-length", None)
            counters = json.loads(body) if body else {}
            normalized_body = json.dumps({name: type(value).__name__ for name, value in counters.items()},
                                         sort_keys=True).encode()
        elif path in {
            "/api/v1/creature",
            f"/api/v1/creature/{FIXTURE_ID}",
            "/api/v1/fixture",
            f"/api/v1/fixture/{FIXTURE_ID}",
            "/__transport_differential_missing__",
        }:
            selected_headers.pop("content-length", None)
            envelope = json.loads(body) if body else {}
            normalized_body = json.dumps(
                {name: envelope.get(name) for name in ("code", "status")}, sort_keys=True
            ).encode()
        snapshot[(method, path)] = (
            status,
            selected_headers,
            normalized_body,
        )
    return snapshot


def check_differential_parity(
    uwebsockets: dict[tuple[str, str], tuple[int, dict[str, str], bytes]],
    oatpp: dict[tuple[str, str], tuple[int, dict[str, str], bytes]],
) -> None:
    require(uwebsockets.keys() == oatpp.keys(), "transport differential case sets do not match")
    mismatches = []
    for case in uwebsockets:
        if uwebsockets[case] != oatpp[case]:
            mismatches.append(f"{case[0]} {case[1]}: uWS={uwebsockets[case]!r}; oat++={oatpp[case]!r}")
    require(not mismatches, "transport differential mismatch:\n" + "\n".join(mismatches))


def check_dead_mongo_deadline(server: ProductionServer) -> None:
    started = time.monotonic()
    code, _, raw = contract.http_request(server.config, "GET", f"/api/v1/fixture/{FIXTURE_ID}")
    elapsed = time.monotonic() - started
    require(code == 500, f"dead Mongo fixture read returned {code}: {raw[:512]!r}")
    require(elapsed < 1.5, f"dead Mongo fixture read exceeded its bounded deadline ({elapsed:.3f}s)")


def check_disconnect_is_safe(server: ProductionServer) -> None:
    connection = contract.open_socket(server.config)
    request = (
        f"GET /api/v1/fixture/{FIXTURE_ID} HTTP/1.1\r\n"
        f"Host: {server.config.host_header}\r\n"
        "Connection: close\r\n\r\n"
    ).encode("ascii")
    connection.sendall(request)
    connection.close()
    time.sleep(0.1)
    code, _, _ = contract.http_request(server.config, "GET", "/api/v1/health")
    require(code == 200, "server did not survive a disconnect during Mongo work")


def check_saturation_isolated_from_loop(server: ProductionServer) -> None:
    start = threading.Event()

    def fixture_read(index: int, wait_for_start: bool = True) -> int:
        if wait_for_start:
            start.wait()
        headers = {"traceparent": otel_gate.TRACEPARENT} if index == 0 else None
        code, _, _ = contract.http_request(
            server.config, "GET", f"/api/v1/fixture/{FIXTURE_ID}", headers=headers
        )
        return code

    with concurrent.futures.ThreadPoolExecutor(max_workers=REQUEST_COUNT) as callers:
        # Admit the traced request before opening the floodgate so it exercises
        # the full service/DB path rather than becoming one of the expected 503s.
        traced = callers.submit(fixture_read, 0, False)
        time.sleep(0.03)
        futures = [traced, *[callers.submit(fixture_read, index) for index in range(1, REQUEST_COUNT)]]
        start.set()
        time.sleep(0.08)
        health_started = time.monotonic()
        health_code, _, _ = contract.http_request(server.config, "GET", "/api/v1/health")
        health_elapsed = time.monotonic() - health_started
        statuses = [future.result(timeout=4.0) for future in futures]

    require(health_code == 200, "health failed while the application executor was saturated")
    require(health_elapsed < 0.2, f"Mongo work stalled the transport loop ({health_elapsed:.3f}s)")
    require(503 in statuses, "bounded application queue did not reject overload")
    require(set(statuses).issubset({500, 503}), f"unexpected overload statuses: {sorted(set(statuses))}")
    final_health, _, _ = contract.http_request(server.config, "GET", "/api/v1/health")
    require(final_health == 200, "server did not recover after the Mongo saturation workload")


def check_trace_hierarchy(capture: otel_gate.OtlpCapture) -> None:
    expected = {
        "GET /api/v1/fixture/{fixtureId}",
        "http.application",
        "DmxFixtureService.getFixture",
        "Database.getFixture",
    }
    spans = capture.wait_for_spans(expected, timeout=12)
    relevant = [span for span in spans if span.get("trace_id") == otel_gate.TRACE_ID]
    by_name = {span.get("name"): span for span in relevant}
    missing = expected - by_name.keys()
    require(not missing, f"traced production request is missing spans: {sorted(missing)}")

    request = by_name["GET /api/v1/fixture/{fixtureId}"]
    application = by_name["http.application"]
    service = by_name["DmxFixtureService.getFixture"]
    database = by_name["Database.getFixture"]
    require(request.get("parent_span_id") == otel_gate.REMOTE_PARENT_ID, "remote request parent was not preserved")
    require(application.get("parent_span_id") == request.get("span_id"), "application span is not under request")
    require(service.get("parent_span_id") == application.get("span_id"), "service span is not under application")
    require(database.get("parent_span_id") == service.get("span_id"), "database span is not under service")
    require(request["attributes"].get("transport.framework") == "uwebsockets", "transport attribute is missing")
    require(request["attributes"].get("transport.outcome") == "response_completed", "request outcome is missing")
    require(request["attributes"].get("http.status_code") == 500, "dead-Mongo HTTP status was not traced")
    require(service["attributes"].get("fixture.id") == FIXTURE_ID, "fixture ID was not attached to service span")
    require(database["attributes"].get("database.system") == "mongodb", "database system attribute is missing")


def check_shutdown_with_inflight_mongo(server: ProductionServer) -> float:
    connections: list[socket.socket] = []
    try:
        websocket, _ = open_websocket(server)
        connections.append(websocket)
        for _ in range(4):
            connection = contract.open_socket(server.config)
            connection.sendall(
                (
                    f"GET /api/v1/fixture/{FIXTURE_ID} HTTP/1.1\r\n"
                    f"Host: {server.config.host_header}\r\n"
                    "Connection: close\r\n\r\n"
                ).encode("ascii")
            )
            connections.append(connection)
        time.sleep(0.05)
        return server.stop(timeout=2.0)
    finally:
        for connection in connections:
            connection.close()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", required=True, type=Path)
    parser.add_argument("--network-device", required=True, help="loopback network interface name (for example lo0 or lo)")
    arguments = parser.parse_args()
    executable = arguments.server.resolve()
    if not executable.is_file():
        parser.error(f"server executable not found: {executable}")

    servers: list[ProductionServer] = []
    capture = otel_gate.OtlpCapture()
    receiver = http.server.ThreadingHTTPServer(("127.0.0.1", 4318), capture.handler())
    receiver_thread = threading.Thread(target=receiver.serve_forever, daemon=True)
    receiver_thread.start()
    try:
        # No selector here: this is the deployable-binary assertion that the
        # process now boots uWebSockets by default.
        uwebsockets = ProductionServer(executable, arguments.network_device, None)
        servers.append(uwebsockets)
        check_websocket_contract(uwebsockets, require_message_limit=True)
        check_websocket_trace(capture)
        # Saturation must be the first Mongo workload: the watchdog marks Mongo
        # unpingable after its first bounded failure, after which reads
        # short-circuit and can no longer fill the application executor.
        check_saturation_isolated_from_loop(uwebsockets)
        check_trace_hierarchy(capture)
        check_disconnect_is_safe(uwebsockets)
        check_static_contract(uwebsockets)
        check_dead_mongo_deadline(uwebsockets)
        uwebsockets_snapshot = differential_snapshot(uwebsockets)
        regular_uwebsockets_shutdown = uwebsockets.stop(timeout=2.0)
        require(
            regular_uwebsockets_shutdown < 2.0,
            f"uWebSockets shutdown exceeded two seconds ({regular_uwebsockets_shutdown:.3f}s)",
        )

        shutdown_server = ProductionServer(executable, arguments.network_device, "uwebsockets")
        servers.append(shutdown_server)
        uwebsockets_shutdown = check_shutdown_with_inflight_mongo(shutdown_server)
        require(
            uwebsockets_shutdown < 2.0,
            f"uWebSockets shutdown exceeded the two-second budget ({uwebsockets_shutdown:.3f}s)",
        )

        oatpp = ProductionServer(executable, arguments.network_device, "oatpp")
        servers.append(oatpp)
        check_websocket_contract(oatpp, require_message_limit=False)
        oatpp_snapshot = differential_snapshot(oatpp)
        check_differential_parity(uwebsockets_snapshot, oatpp_snapshot)
        print(
            "PASS: default uWebSockets/explicit oat++ rollback differential, WebSocket ordering/tracing, "
            "API browser, dead-Mongo deadline, disconnect, saturation isolation, and "
            f"shutdown (uWS {uwebsockets_shutdown:.3f}s; "
            "oat++ oracle terminated after snapshot)"
        )
        return 0
    except BaseException as error:
        print(f"FAIL: {error}", file=sys.stderr)
        for server in servers:
            print(f"--- {server.transport} server log tail ---", file=sys.stderr)
            print(server.logs()[-12000:], file=sys.stderr)
        return 1
    finally:
        for server in servers:
            server.close()
        receiver.shutdown()
        receiver.server_close()


if __name__ == "__main__":
    raise SystemExit(main())
