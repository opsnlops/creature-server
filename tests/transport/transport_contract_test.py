#!/usr/bin/env python3
"""Black-box transport contract for Creature Server.

The suite intentionally uses only Python's standard library. It can exercise
the current oat++ server and future transport spikes without importing or
linking either implementation.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import http.client
import ipaddress
import io
import json
import math
import os
import socket
import ssl
import struct
import sys
import time
import unittest
import urllib.parse
import uuid
from dataclasses import dataclass
from typing import Callable


DEFAULT_TIMEOUT_SECONDS = 5.0
MAX_FIXTURE_CONFIG_REQUEST_BODY_BYTES = 1024 * 1024
MAX_HTTP_HEADER_BYTES = 64 * 1024
MAX_JSON_RESPONSE_BYTES = 2 * 1024 * 1024
MAX_SOUND_BODY_BYTES = 32 * 1024 * 1024
WEBSOCKET_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


@dataclass(frozen=True)
class ContractConfig:
    scheme: str
    host: str
    port: int
    base_path: str
    timeout: float
    allow_remote: bool
    include_mutating: bool
    sound_file: str | None
    require_byte_ranges: bool
    require_fragmented_message_limit: bool

    @property
    def host_header(self) -> str:
        default_port = 443 if self.scheme == "https" else 80
        bracketed_host = f"[{self.host}]" if ":" in self.host else self.host
        return bracketed_host if self.port == default_port else f"{bracketed_host}:{self.port}"

    def path(self, suffix: str) -> str:
        return f"{self.base_path.rstrip('/')}/{suffix.lstrip('/')}" or "/"


CONFIG: ContractConfig | None = None


def parse_base_url(base_url: str, allow_remote: bool) -> ContractConfig:
    parsed = urllib.parse.urlsplit(base_url)
    if parsed.scheme not in {"http", "https"}:
        raise ValueError("base URL scheme must be http or https")
    if not parsed.hostname:
        raise ValueError("base URL must include a hostname")
    if parsed.query or parsed.fragment:
        raise ValueError("base URL must not include a query or fragment")

    try:
        addresses = {item[4][0] for item in socket.getaddrinfo(parsed.hostname, None)}
    except socket.gaierror as error:
        raise ValueError(f"could not resolve base URL hostname: {error}") from error
    loopback = all(address == "::1" or address.startswith("127.") for address in addresses)
    if not loopback and not allow_remote:
        raise ValueError("refusing to target a non-loopback server without --allow-remote")

    return ContractConfig(
        scheme=parsed.scheme,
        host=parsed.hostname,
        port=parsed.port or (443 if parsed.scheme == "https" else 80),
        base_path=parsed.path.rstrip("/"),
        timeout=DEFAULT_TIMEOUT_SECONDS,
        allow_remote=allow_remote,
        include_mutating=False,
        sound_file=None,
        require_byte_ranges=False,
        require_fragmented_message_limit=False,
    )


def verify_peer(config: ContractConfig, connection: socket.socket) -> None:
    if config.allow_remote:
        return
    peer_address = connection.getpeername()[0]
    if not ipaddress.ip_address(peer_address).is_loopback:
        connection.close()
        raise ConnectionError(f"refusing non-loopback peer address {peer_address}")


def open_socket(config: ContractConfig) -> socket.socket:
    connection = socket.create_connection((config.host, config.port), timeout=config.timeout)
    if config.scheme == "https":
        context = ssl.create_default_context()
        connection = context.wrap_socket(connection, server_hostname=config.host)
    verify_peer(config, connection)
    return connection


def http_request(
    config: ContractConfig,
    method: str,
    path: str,
    *,
    body: bytes | None = None,
    headers: dict[str, str] | None = None,
    max_response_bytes: int = MAX_JSON_RESPONSE_BYTES,
) -> tuple[int, dict[str, str], bytes]:
    connection_type = http.client.HTTPSConnection if config.scheme == "https" else http.client.HTTPConnection
    connection = connection_type(config.host, config.port, timeout=config.timeout)
    try:
        connection.connect()
        if connection.sock is None:
            raise ConnectionError("HTTP connection did not expose its connected socket")
        verify_peer(config, connection.sock)
        request_headers = {"Accept": "application/json", "User-Agent": "creature-transport-contract/1"}
        if headers:
            request_headers.update(headers)
        connection.request(method, config.path(path), body=body, headers=request_headers)
        response = connection.getresponse()
        response_body = response.read(max_response_bytes + 1)
        if len(response_body) > max_response_bytes:
            raise ValueError(f"response exceeded {max_response_bytes} byte harness limit")
        return response.status, {key.lower(): value for key, value in response.getheaders()}, response_body
    finally:
        connection.close()


def decode_json(test: unittest.TestCase, headers: dict[str, str], body: bytes) -> object:
    test.assertEqual(headers.get("content-type"), "application/json; charset=utf-8")
    try:
        return json.loads(body)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        test.fail(f"response is not valid UTF-8 JSON: {error}; body={body[:512]!r}")


def assert_status_envelope(test: unittest.TestCase, body: object, code: int, status: str) -> None:
    test.assertIsInstance(body, dict)
    test.assertEqual(body.get("code"), code)
    test.assertEqual(body.get("status"), status)
    test.assertIsInstance(body.get("message"), str)


class HttpSocketReader:
    def __init__(self, connection: socket.socket):
        self.connection = connection
        self.buffer = bytearray()

    def _fill(self, minimum: int = 1) -> None:
        while len(self.buffer) < minimum:
            chunk = self.connection.recv(65536)
            if not chunk:
                raise EOFError("connection closed before the response completed")
            self.buffer.extend(chunk)

    def read_exactly(self, size: int) -> bytes:
        self._fill(size)
        value = bytes(self.buffer[:size])
        del self.buffer[:size]
        return value

    def read_until(self, delimiter: bytes, *, maximum: int | None = None) -> bytes:
        while True:
            position = self.buffer.find(delimiter)
            if position >= 0:
                end = position + len(delimiter)
                if maximum is not None and end > maximum:
                    raise ValueError(f"framed field exceeded {maximum} byte harness limit")
                value = bytes(self.buffer[:end])
                del self.buffer[:end]
                return value
            if maximum is not None and len(self.buffer) >= maximum:
                raise ValueError(f"framed field exceeded {maximum} byte harness limit")
            self._fill(len(self.buffer) + 1)

    def read_http_response(self) -> tuple[int, dict[str, str], bytes]:
        head = self.read_until(b"\r\n\r\n", maximum=MAX_HTTP_HEADER_BYTES)
        lines = head[:-4].split(b"\r\n")
        status_parts = lines[0].decode("ascii").split(" ", 2)
        if len(status_parts) < 2 or not status_parts[1].isdigit():
            raise ValueError(f"invalid HTTP status line: {lines[0]!r}")
        headers: dict[str, str] = {}
        for line in lines[1:]:
            key, separator, value = line.partition(b":")
            if not separator:
                raise ValueError(f"invalid HTTP header: {line!r}")
            headers[key.decode("ascii").lower()] = value.decode("latin-1").strip()

        if "chunked" in headers.get("transfer-encoding", "").lower():
            chunks: list[bytes] = []
            total_size = 0
            while True:
                size_line = self.read_until(b"\r\n", maximum=MAX_HTTP_HEADER_BYTES)[:-2].split(b";", 1)[0]
                chunk_size = int(size_line, 16)
                if chunk_size == 0:
                    trailer_bytes = 0
                    trailer_count = 0
                    while True:
                        trailer = self.read_until(b"\r\n", maximum=MAX_HTTP_HEADER_BYTES)
                        trailer_bytes += len(trailer)
                        trailer_count += 1
                        if trailer_bytes > MAX_HTTP_HEADER_BYTES or trailer_count > 100:
                            raise ValueError("response trailers exceeded harness limits")
                        if trailer == b"\r\n":
                            break
                    break
                total_size += chunk_size
                if total_size > MAX_JSON_RESPONSE_BYTES:
                    raise ValueError(f"response exceeded {MAX_JSON_RESPONSE_BYTES} byte harness limit")
                chunks.append(self.read_exactly(chunk_size))
                if self.read_exactly(2) != b"\r\n":
                    raise ValueError("chunk did not end with CRLF")
            body = b"".join(chunks)
        else:
            content_length = int(headers.get("content-length", "0"))
            if content_length > MAX_JSON_RESPONSE_BYTES:
                raise ValueError(f"response exceeded {MAX_JSON_RESPONSE_BYTES} byte harness limit")
            body = self.read_exactly(content_length)
        return int(status_parts[1]), headers, body


def encode_client_frame(opcode: int, payload: bytes, *, final: bool = True, mask: bytes | None = None) -> bytes:
    mask = mask or os.urandom(4)
    if len(mask) != 4:
        raise ValueError("WebSocket mask must contain four bytes")
    first = (0x80 if final else 0) | opcode
    length = len(payload)
    if length < 126:
        header = bytes((first, 0x80 | length))
    elif length <= 0xFFFF:
        header = bytes((first, 0x80 | 126)) + struct.pack("!H", length)
    else:
        header = bytes((first, 0x80 | 127)) + struct.pack("!Q", length)
    masked = bytes(value ^ mask[index % 4] for index, value in enumerate(payload))
    return header + mask + masked


def read_server_frame(reader: HttpSocketReader) -> tuple[bool, int, bytes]:
    first, second = reader.read_exactly(2)
    final = bool(first & 0x80)
    opcode = first & 0x0F
    masked = bool(second & 0x80)
    length = second & 0x7F
    if length == 126:
        length = struct.unpack("!H", reader.read_exactly(2))[0]
    elif length == 127:
        length = struct.unpack("!Q", reader.read_exactly(8))[0]
    if masked:
        raise ValueError("server-to-client WebSocket frames must not be masked")
    if length > 2 * 1024 * 1024:
        raise ValueError(f"refusing oversized server WebSocket frame ({length} bytes)")
    return final, opcode, reader.read_exactly(length)


def read_matching_server_frame(
    connection: socket.socket,
    reader: HttpSocketReader,
    predicate: Callable[[bool, int, bytes], bool],
    *,
    timeout: float,
) -> tuple[bool, int, bytes]:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        connection.settimeout(max(0.1, deadline - time.monotonic()))
        frame = read_server_frame(reader)
        final, opcode, payload = frame
        if opcode == 0x9:
            connection.sendall(encode_client_frame(0xA, payload))
            continue
        if predicate(final, opcode, payload):
            return frame
    raise TimeoutError("no matching WebSocket frame arrived before the deadline")


class TransportContractTests(unittest.TestCase):
    @property
    def config(self) -> ContractConfig:
        if CONFIG is None:
            self.fail("transport test configuration was not initialized")
        return CONFIG

    def test_health_uses_canonical_json_envelope(self) -> None:
        code, headers, raw = http_request(
            self.config,
            "GET",
            "/api/v1/health",
            headers={"traceparent": "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01"},
        )
        self.assertEqual(code, 200)
        body = decode_json(self, headers, raw)
        self.assertEqual(body, {"status": "ok", "code": 200, "message": "Server is operational"})

    def test_head_matches_health_get_without_a_body(self) -> None:
        get_code, get_headers, get_body = http_request(self.config, "GET", "/api/v1/health")
        head_code, head_headers, head_body = http_request(self.config, "HEAD", "/api/v1/health")
        self.assertEqual(head_code, get_code)
        self.assertEqual(head_headers.get("content-type"), get_headers.get("content-type"))
        self.assertEqual(head_headers.get("content-length"), str(len(get_body)))
        self.assertEqual(head_body, b"")

    def test_unknown_route_uses_canonical_not_found_envelope(self) -> None:
        path = f"/__transport_contract_missing__/{uuid.uuid4()}"
        code, headers, raw = http_request(self.config, "GET", path)
        self.assertEqual(code, 404)
        assert_status_envelope(self, decode_json(self, headers, raw), 404, "not_found")

    def test_invalid_uuid_is_rejected_before_database_lookup(self) -> None:
        code, headers, raw = http_request(self.config, "GET", "/api/v1/fixture/not-a-uuid")
        self.assertEqual(code, 400)
        assert_status_envelope(self, decode_json(self, headers, raw), 400, "error")

    def test_fixture_validation_keeps_application_errors_in_a_200_result(self) -> None:
        code, headers, raw = http_request(
            self.config,
            "POST",
            "/api/v1/fixture/validate",
            body=b'{"unterminated',
            headers={"Content-Type": "application/json"},
        )
        self.assertEqual(code, 200)
        body = decode_json(self, headers, raw)
        self.assertEqual(body.get("valid"), False)
        self.assertEqual(body.get("missing_creature_ids"), [])
        self.assertTrue(body.get("error_messages"))

    def test_fixture_validation_rejects_oversized_body(self) -> None:
        oversized = b" " * (MAX_FIXTURE_CONFIG_REQUEST_BODY_BYTES + 1)
        code, headers, raw = http_request(
            self.config,
            "POST",
            "/api/v1/fixture/validate",
            body=oversized,
            headers={"Content-Type": "application/json"},
        )
        self.assertEqual(code, 413)
        assert_status_envelope(self, decode_json(self, headers, raw), 413, "error")

    def test_fixture_validation_rejects_oversized_chunked_body(self) -> None:
        connection = open_socket(self.config)
        try:
            fixture_path = self.config.path("/api/v1/fixture/validate")
            request = (
                f"POST {fixture_path} HTTP/1.1\r\n"
                f"Host: {self.config.host_header}\r\n"
                "Content-Type: application/json\r\n"
                "Transfer-Encoding: chunked\r\n"
                "Connection: close\r\n\r\n"
            ).encode("ascii")
            chunk = b" " * (MAX_FIXTURE_CONFIG_REQUEST_BODY_BYTES // 4)
            framed_chunks = b"".join(f"{len(chunk):x}\r\n".encode("ascii") + chunk + b"\r\n" for _ in range(4))
            # Exceed the cumulative limit without terminating the chunked body.
            # A 413 here proves enforcement happens while bytes are read.
            connection.sendall(request + framed_chunks + b"1\r\n \r\n")
            code, headers, raw = HttpSocketReader(connection).read_http_response()
            self.assertEqual(code, 413)
            assert_status_envelope(self, decode_json(self, headers, raw), 413, "error")
        finally:
            connection.close()

    def test_malformed_fixture_upsert_is_a_client_error_and_server_survives(self) -> None:
        code, headers, raw = http_request(
            self.config,
            "POST",
            "/api/v1/fixture",
            body=b'{"unterminated',
            headers={"Content-Type": "application/json"},
        )
        self.assertEqual(code, 400)
        assert_status_envelope(self, decode_json(self, headers, raw), 400, "error")

        health_code, _, _ = http_request(self.config, "GET", "/api/v1/health")
        self.assertEqual(health_code, 200)

    def assert_keep_alive_survives_unexpected_get_body(self, body_headers: str, framed_body: bytes) -> None:
        connection = open_socket(self.config)
        try:
            reader = HttpSocketReader(connection)
            health_path = self.config.path("/api/v1/health")
            requests = (
                f"GET {health_path} HTTP/1.1\r\n"
                f"Host: {self.config.host_header}\r\n"
                f"{body_headers}"
                "Connection: keep-alive\r\n\r\n"
            ).encode("ascii") + framed_body + (
                f"GET {health_path} HTTP/1.1\r\n"
                f"Host: {self.config.host_header}\r\n"
                "Connection: close\r\n\r\n"
            ).encode("ascii")
            connection.sendall(requests)
            first = reader.read_http_response()
            second = reader.read_http_response()
            self.assertEqual(first[0], 200)
            self.assertEqual(second[0], 200)
            self.assertEqual(json.loads(second[2]), {"status": "ok", "code": 200, "message": "Server is operational"})
        finally:
            connection.close()

    def test_fixed_length_get_body_does_not_corrupt_next_keep_alive_request(self) -> None:
        body = b'{"ignored":true}'
        self.assert_keep_alive_survives_unexpected_get_body(f"Content-Length: {len(body)}\r\n", body)

    def test_chunked_get_body_does_not_corrupt_next_keep_alive_request(self) -> None:
        body = b'{"ignored":true}'
        framed = f"{len(body):x}\r\n".encode("ascii") + body + b"\r\n0\r\n\r\n"
        self.assert_keep_alive_survives_unexpected_get_body("Transfer-Encoding: chunked\r\n", framed)

    def open_websocket(self) -> tuple[socket.socket, HttpSocketReader]:
        connection = open_socket(self.config)
        try:
            reader = HttpSocketReader(connection)
            websocket_key = base64.b64encode(os.urandom(16)).decode("ascii")
            request = (
                f"GET {self.config.path('/api/v1/websocket')} HTTP/1.1\r\n"
                f"Host: {self.config.host_header}\r\n"
                "Upgrade: websocket\r\n"
                "Connection: Upgrade\r\n"
                f"Sec-WebSocket-Key: {websocket_key}\r\n"
                "Sec-WebSocket-Version: 13\r\n\r\n"
            ).encode("ascii")
            connection.sendall(request)
            code, headers, body = reader.read_http_response()
            self.assertEqual(code, 101)
            self.assertEqual(body, b"")
            self.assertEqual(headers.get("upgrade", "").lower(), "websocket")
            self.assertIn("upgrade", headers.get("connection", "").lower())
            expected_accept = base64.b64encode(
                hashlib.sha1((websocket_key + WEBSOCKET_GUID).encode("ascii")).digest()
            ).decode("ascii")
            self.assertEqual(headers.get("sec-websocket-accept"), expected_accept)
            return connection, reader
        except BaseException:
            connection.close()
            raise

    def test_websocket_upgrade_fragmentation_and_ping_pong(self) -> None:
        def is_malformed_notice(final: bool, opcode: int, payload: bytes) -> bool:
            if not final or opcode != 0x1:
                return False
            try:
                candidate = json.loads(payload)
            except (UnicodeDecodeError, json.JSONDecodeError):
                return False
            return (
                candidate.get("command") == "notice"
                and candidate.get("payload", {}).get("message") == "Dropped malformed WebSocket message."
            )

        connection, reader = self.open_websocket()
        try:
            connection.sendall(encode_client_frame(0x1, b'{"command":', final=False))
            connection.sendall(encode_client_frame(0x0, b'"unterminated', final=True))
            final, opcode, payload = read_matching_server_frame(
                connection, reader, is_malformed_notice, timeout=self.config.timeout
            )
            self.assertTrue(final)
            self.assertEqual(opcode, 0x1)
            notice = json.loads(payload)
            self.assertEqual(notice.get("command"), "notice")
            self.assertEqual(notice.get("payload", {}).get("message"), "Dropped malformed WebSocket message.")

            ping_payload = b"transport-contract"
            connection.sendall(encode_client_frame(0x9, ping_payload))
            final, opcode, payload = read_matching_server_frame(
                connection,
                reader,
                lambda candidate_final, candidate_opcode, candidate_payload: candidate_final
                and candidate_opcode == 0xA
                and candidate_payload == ping_payload,
                timeout=self.config.timeout,
            )
            self.assertTrue(final)
            self.assertEqual(opcode, 0xA)
            self.assertEqual(payload, ping_payload)
        finally:
            connection.close()

        connection, reader = self.open_websocket()
        try:
            # Exactly 64 KiB spread across modest frames is accepted. This is
            # the positive control proving the close below is based on the
            # aggregate logical-message size, not an individual frame.
            for index in range(8):
                connection.sendall(
                    encode_client_frame(0x1 if index == 0 else 0x0, b"x" * (8 * 1024), final=index == 7)
                )
            read_matching_server_frame(connection, reader, is_malformed_notice, timeout=self.config.timeout)
        finally:
            connection.close()

        if not self.config.require_fragmented_message_limit:
            return

        connection, reader = self.open_websocket()
        try:
            # Candidate transports must cap the aggregate logical message, not
            # merely each frame.
            for index in range(8):
                connection.sendall(
                    encode_client_frame(0x1 if index == 0 else 0x0, b"x" * (8 * 1024), final=False)
                )
            closed = False
            try:
                connection.sendall(encode_client_frame(0x0, b"x", final=True))
            except (BrokenPipeError, ConnectionResetError):
                closed = True
            deadline = time.monotonic() + self.config.timeout
            try:
                while not closed:
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        break
                    connection.settimeout(remaining)
                    _, candidate_opcode, candidate_payload = read_server_frame(reader)
                    if candidate_opcode == 0x8:
                        closed = True
                        break
                    if candidate_opcode == 0x9:
                        connection.sendall(encode_client_frame(0xA, candidate_payload))
            except TimeoutError:
                pass
            except (ConnectionError, EOFError):
                closed = True
            self.assertTrue(closed, "server kept an oversized WebSocket message connection open")
        finally:
            connection.close()

    def test_fixture_crud_round_trip(self) -> None:
        if not self.config.include_mutating:
            self.skipTest("enable with --include-mutating against a disposable database")

        fixture_id = str(uuid.uuid4())
        fixture = {
            "id": fixture_id,
            "name": f"Transport Contract {fixture_id}",
            "type": "generic",
            "channel_offset": 500,
            "channels": [{"offset": 0, "name": "value", "kind": "generic"}],
            "patterns": [],
            "bindings": [],
        }

        needs_cleanup = True

        def delete_fixture() -> None:
            if not needs_cleanup:
                return
            cleanup_code, _, _ = http_request(self.config, "DELETE", f"/api/v1/fixture/{fixture_id}")
            self.assertIn(cleanup_code, (200, 404), "fixture cleanup failed")

        self.addCleanup(delete_fixture)
        encoded = json.dumps(fixture, separators=(",", ":")).encode("utf-8")
        code, headers, raw = http_request(
            self.config,
            "POST",
            "/api/v1/fixture",
            body=encoded,
            headers={"Content-Type": "application/json"},
        )
        self.assertEqual(code, 200)
        self.assertEqual(decode_json(self, headers, raw).get("id"), fixture_id)

        code, headers, raw = http_request(self.config, "GET", f"/api/v1/fixture/{fixture_id}")
        self.assertEqual(code, 200)
        fetched = decode_json(self, headers, raw)
        self.assertEqual(fetched.get("id"), fixture_id)
        self.assertEqual(fetched.get("name"), fixture["name"])

        code, headers, raw = http_request(self.config, "DELETE", f"/api/v1/fixture/{fixture_id}")
        self.assertEqual(code, 200)
        assert_status_envelope(self, decode_json(self, headers, raw), 200, "ok")
        needs_cleanup = False

        code, headers, raw = http_request(self.config, "GET", f"/api/v1/fixture/{fixture_id}")
        self.assertEqual(code, 404)
        assert_status_envelope(self, decode_json(self, headers, raw), 404, "not_found")

    def test_stored_sound_head_and_optional_range(self) -> None:
        if not self.config.sound_file:
            self.skipTest("enable with --sound-file NAME")
        path = "/api/v1/sound/" + urllib.parse.quote(self.config.sound_file, safe="")
        head_code, head_headers, head_body = http_request(
            self.config, "HEAD", path, max_response_bytes=MAX_SOUND_BODY_BYTES
        )
        self.assertEqual(head_code, 200)
        content_length = head_headers.get("content-length")
        self.assertIsNotNone(content_length)
        self.assertLessEqual(
            int(content_length),
            MAX_SOUND_BODY_BYTES,
            "choose a sound smaller than 32 MiB for this in-memory contract check",
        )

        get_code, get_headers, get_body = http_request(
            self.config, "GET", path, max_response_bytes=MAX_SOUND_BODY_BYTES
        )
        self.assertEqual(get_code, head_code)
        self.assertEqual(head_headers.get("content-type"), get_headers.get("content-type"))
        self.assertEqual(head_headers.get("content-length"), str(len(get_body)))
        self.assertEqual(head_body, b"")

        if self.config.require_byte_ranges:
            range_code, range_headers, range_body = http_request(
                self.config,
                "GET",
                path,
                headers={"Range": "bytes=0-0"},
                max_response_bytes=MAX_SOUND_BODY_BYTES,
            )
            self.assertEqual(range_code, 206)
            self.assertEqual(range_headers.get("content-range"), f"bytes 0-0/{len(get_body)}")
            self.assertEqual(range_body, get_body[:1])


class ProtocolHelperTests(unittest.TestCase):
    def test_client_frame_is_masked_and_round_trips(self) -> None:
        payload = b"hello"
        mask = b"\x01\x02\x03\x04"
        frame = encode_client_frame(0x1, payload, mask=mask)
        self.assertEqual(frame[:2], bytes((0x81, 0x80 | len(payload))))
        self.assertEqual(frame[2:6], mask)
        self.assertEqual(bytes(value ^ mask[index % 4] for index, value in enumerate(frame[6:])), payload)

    def test_http_reader_handles_fixed_and_chunked_responses(self) -> None:
        first, second = socket.socketpair()
        try:
            second.sendall(
                b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{}"
                b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\n[]\r\n0\r\n\r\n"
            )
            reader = HttpSocketReader(first)
            self.assertEqual(reader.read_http_response()[2], b"{}")
            self.assertEqual(reader.read_http_response()[2], b"[]")
        finally:
            first.close()
            second.close()


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base-url", help="server root, for example http://127.0.0.1:8000")
    parser.add_argument("--allow-remote", action="store_true", help="allow a non-loopback target")
    parser.add_argument(
        "--include-mutating",
        action="store_true",
        help="exercise fixture create/get/delete against a disposable database",
    )
    parser.add_argument("--sound-file", help="stored sound filename used for GET/HEAD checks")
    parser.add_argument(
        "--require-byte-ranges",
        action="store_true",
        help="require a one-byte Range request to return 206 (requires --sound-file)",
    )
    parser.add_argument(
        "--require-fragmented-message-limit",
        action="store_true",
        help="require the 64 KiB WebSocket limit across fragmented messages",
    )
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT_SECONDS)
    parser.add_argument("--self-test", action="store_true", help="test the protocol harness without a server")
    return parser


def run_suite(test_case: type[unittest.TestCase]) -> bool:
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(test_case)
    return unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful()


def main(argv: list[str]) -> int:
    arguments = make_parser().parse_args(argv)
    if arguments.self_test:
        return 0 if run_suite(ProtocolHelperTests) else 1
    if not arguments.base_url:
        print("error: --base-url is required unless --self-test is used", file=sys.stderr)
        return 2
    if arguments.require_byte_ranges and not arguments.sound_file:
        print("error: --require-byte-ranges requires --sound-file", file=sys.stderr)
        return 2
    if not math.isfinite(arguments.timeout) or not 0.1 <= arguments.timeout <= 300:
        print("error: --timeout must be between 0.1 and 300 seconds", file=sys.stderr)
        return 2

    try:
        config = parse_base_url(arguments.base_url, arguments.allow_remote)
    except ValueError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    global CONFIG
    CONFIG = ContractConfig(
        scheme=config.scheme,
        host=config.host,
        port=config.port,
        base_path=config.base_path,
        timeout=arguments.timeout,
        allow_remote=config.allow_remote,
        include_mutating=arguments.include_mutating,
        sound_file=arguments.sound_file,
        require_byte_ranges=arguments.require_byte_ranges,
        require_fragmented_message_limit=arguments.require_fragmented_message_limit,
    )
    return 0 if run_suite(TransportContractTests) else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
