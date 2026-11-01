#!/usr/bin/env python3

import argparse
import base64
import http.server
import json
import os
import signal
import socket
import struct
import subprocess
import tempfile
import threading
import time
import urllib.request


FIXTURE_ID = "8e3a4b5c-1d2f-4e6a-9b0c-7f8e9d0a1b2c"
TRACE_ID = "4bf92f3577b34da6a3ce929d0e0e4736"
REMOTE_PARENT_ID = "00f067aa0ba902b7"
TRACEPARENT = f"00-{TRACE_ID}-{REMOTE_PARENT_ID}-01"


def read_varint(data, offset):
    value = 0
    shift = 0
    while True:
        byte = data[offset]
        offset += 1
        value |= (byte & 0x7F) << shift
        if byte < 0x80:
            return value, offset
        shift += 7


def fields(data):
    offset = 0
    while offset < len(data):
        tag, offset = read_varint(data, offset)
        number = tag >> 3
        wire_type = tag & 7
        if wire_type == 0:
            value, offset = read_varint(data, offset)
        elif wire_type == 1:
            value = data[offset : offset + 8]
            offset += 8
        elif wire_type == 2:
            length, offset = read_varint(data, offset)
            value = data[offset : offset + length]
            offset += length
        elif wire_type == 5:
            value = data[offset : offset + 4]
            offset += 4
        else:
            raise AssertionError(f"unsupported protobuf wire type {wire_type}")
        yield number, wire_type, value


def length_delimited(data, field_number):
    return [value for number, wire_type, value in fields(data) if number == field_number and wire_type == 2]


def parse_any_value(data):
    for number, wire_type, value in fields(data):
        if number == 1 and wire_type == 2:
            return value.decode()
        if number in (2, 3) and wire_type == 0:
            return value
        if number == 4 and wire_type == 1:
            return struct.unpack("<d", value)[0]
    return None


def parse_attributes(span_data):
    result = {}
    for key_value in length_delimited(span_data, 9):
        keys = length_delimited(key_value, 1)
        values = length_delimited(key_value, 2)
        if keys and values:
            result[keys[0].decode()] = parse_any_value(values[0])
    return result


def parse_span(span_data):
    parsed = {"attributes": parse_attributes(span_data)}
    for number, wire_type, value in fields(span_data):
        if number == 1 and wire_type == 2:
            parsed["trace_id"] = value.hex()
        elif number == 2 and wire_type == 2:
            parsed["span_id"] = value.hex()
        elif number == 4 and wire_type == 2:
            parsed["parent_span_id"] = value.hex()
        elif number == 5 and wire_type == 2:
            parsed["name"] = value.decode()
    return parsed


def parse_export_request(data):
    spans = []
    for resource_spans in length_delimited(data, 1):
        for scope_spans in length_delimited(resource_spans, 2):
            spans.extend(parse_span(span) for span in length_delimited(scope_spans, 2))
    return spans


class OtlpCapture:
    def __init__(self):
        self.trace_bodies = []
        self.condition = threading.Condition()

    def handler(self):
        capture = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_POST(self):
                body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
                if self.path == "/v1/traces":
                    with capture.condition:
                        capture.trace_bodies.append(body)
                        capture.condition.notify_all()
                self.send_response(200)
                self.send_header("Content-Type", "application/x-protobuf")
                self.send_header("Content-Length", "0")
                self.end_headers()

            def log_message(self, *_args):
                return

        return Handler

    def wait_for_spans(self, expected_names, timeout=12):
        deadline = time.monotonic() + timeout
        with self.condition:
            while time.monotonic() < deadline:
                spans = [span for body in self.trace_bodies for span in parse_export_request(body)]
                if expected_names.issubset({span.get("name") for span in spans}):
                    return spans
                self.condition.wait(deadline - time.monotonic())
        raise AssertionError(f"timed out waiting for spans {sorted(expected_names)}")


def wait_for_http(url, timeout=10):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            with urllib.request.urlopen(url, timeout=0.5) as response:
                if response.status == 200:
                    return
        except Exception:
            time.sleep(0.1)
    raise AssertionError(f"server did not become ready: {url}")


def open_websocket(port):
    connection = socket.create_connection(("127.0.0.1", port), timeout=3)
    key = base64.b64encode(os.urandom(16)).decode()
    request = (
        "GET /api/v1/websocket HTTP/1.1\r\n"
        f"Host: 127.0.0.1:{port}\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        f"traceparent: {TRACEPARENT}\r\n\r\n"
    )
    connection.sendall(request.encode())
    response = b""
    while b"\r\n\r\n" not in response:
        response += connection.recv(4096)
    assert response.startswith(b"HTTP/1.1 101")
    return connection


def send_masked_frame(connection, opcode, payload):
    mask = b"gate"
    header = bytes([0x80 | opcode])
    if len(payload) < 126:
        header += bytes([0x80 | len(payload)])
    elif len(payload) <= 65535:
        header += bytes([0x80 | 126]) + len(payload).to_bytes(2, "big")
    else:
        header += bytes([0x80 | 127]) + len(payload).to_bytes(8, "big")
    masked = bytes(value ^ mask[index % 4] for index, value in enumerate(payload))
    connection.sendall(header + mask + masked)


def read_frame(connection):
    first = connection.recv(2)
    assert len(first) == 2
    length = first[1] & 0x7F
    if length == 126:
        length = int.from_bytes(connection.recv(2), "big")
    elif length == 127:
        length = int.from_bytes(connection.recv(8), "big")
    payload = b""
    while len(payload) < length:
        payload += connection.recv(length - len(payload))
    return first[0] & 0x0F, payload


def docker(*arguments, input_text=None):
    return subprocess.run(
        ["docker", *arguments],
        input=input_text,
        text=True,
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )


def wait_for_mongo(container_name, timeout=20):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = subprocess.run(
            ["docker", "exec", container_name, "mongosh", "--quiet", "--eval", "db.runCommand({ping:1}).ok"],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        if result.returncode == 0 and result.stdout.strip() == "1":
            return
        time.sleep(0.25)
    raise AssertionError("MongoDB did not become ready")


def fixture_seed_script():
    fixture = {
        "id": FIXTURE_ID,
        "name": "OTel Gate Spot",
        "type": "light",
        "channel_offset": 500,
        "assigned_universe": 1,
        "channels": [{"offset": 0, "name": "red", "kind": "color_red"}],
        "patterns": [],
        "bindings": [],
    }
    return f'db.getSiblingDB("creature_server").fixtures.insertOne({json.dumps(fixture)})'


def assert_trace(spans):
    relevant = [span for span in spans if span.get("trace_id") == TRACE_ID]
    by_name = {span["name"]: span for span in relevant}
    all_by_name = {span["name"]: span for span in spans}
    request = by_name["GET /api/v1/fixture/{fixtureId}"]
    service = by_name["DmxFixtureService.getFixture"]
    database = by_name["Database.getFixture"]
    mongo = by_name["getFixtureJson.mongoQuery"]

    assert request["parent_span_id"] == REMOTE_PARENT_ID
    assert service["parent_span_id"] == request["span_id"]
    assert database["parent_span_id"] == service["span_id"]

    parent_by_id = {span["span_id"]: span for span in relevant}
    cursor = mongo
    ancestry = []
    while cursor.get("parent_span_id") in parent_by_id:
        cursor = parent_by_id[cursor["parent_span_id"]]
        ancestry.append(cursor["name"])
    assert "Database.getFixture" in ancestry
    assert request["attributes"]["transport.framework"] == "uwebsockets"
    assert request["attributes"]["transport.outcome"] == "response_completed"
    assert request["attributes"]["http.status_code"] == 200
    assert service["attributes"]["fixture.id"] == FIXTURE_ID
    assert database["attributes"]["database.system"] == "mongodb"

    upgrade = by_name["GET /api/v1/websocket"]
    session = all_by_name["WebSocket.session"]
    broadcast_request = by_name["POST /__spike/broadcast"]
    broadcast = by_name["WebSocket.broadcast"]
    assert upgrade["parent_span_id"] == REMOTE_PARENT_ID
    assert upgrade["attributes"]["http.status_code"] == 101
    assert session["attributes"]["error.type"] == "MalformedWebSocketJson"
    assert session["attributes"]["transport.outcome"] == "message_error"
    assert session["attributes"]["websocket.sample_rate"] == 0.0005
    assert broadcast_request["parent_span_id"] == REMOTE_PARENT_ID
    assert broadcast_request["attributes"]["http.status_code"] == 202
    assert broadcast["trace_id"] == TRACE_ID
    assert broadcast["attributes"]["transport.outcome"] == "published"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--server",
        default="build-otel-gate/uwebsockets-otel-gate/creature-server-uwebsockets-otel-gate",
    )
    args = parser.parse_args()

    container_name = f"creature-uws-otel-gate-{os.getpid()}"
    mongo_port = 27029
    server_port = 18088
    capture = OtlpCapture()
    receiver = http.server.ThreadingHTTPServer(("127.0.0.1", 4318), capture.handler())
    receiver_thread = threading.Thread(target=receiver.serve_forever, daemon=True)
    receiver_thread.start()
    server = None

    try:
        docker("run", "-d", "--rm", "--name", container_name, "-p", f"127.0.0.1:{mongo_port}:27017", "mongo:7")
        wait_for_mongo(container_name)
        docker("exec", container_name, "mongosh", "--quiet", "--eval", fixture_seed_script())

        with tempfile.TemporaryDirectory(prefix="creature-uws-otel-sounds-") as sounds:
            environment = os.environ.copy()
            environment.update(
                {
                    "SERVER_PORT": str(server_port),
                    "SPIKE_MONGODB_URI": f"mongodb://127.0.0.1:{mongo_port}/?serverSelectionTimeoutMS=2000",
                }
            )
            server = subprocess.Popen(
                [args.server, "--sounds-location", sounds],
                env=environment,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )
            wait_for_http(f"http://127.0.0.1:{server_port}/api/v1/health")

            request = urllib.request.Request(
                f"http://127.0.0.1:{server_port}/api/v1/fixture/{FIXTURE_ID}",
                headers={"traceparent": TRACEPARENT},
            )
            with urllib.request.urlopen(request, timeout=5) as response:
                body = json.load(response)
                assert response.status == 200
                assert body["id"] == FIXTURE_ID
                assert body["name"] == "OTel Gate Spot"

            websocket = open_websocket(server_port)
            try:
                send_masked_frame(websocket, 1, b"{")
                opcode, notice = read_frame(websocket)
                assert opcode == 1
                assert json.loads(notice)["command"] == "notice"

                broadcast_request = urllib.request.Request(
                    f"http://127.0.0.1:{server_port}/__spike/broadcast",
                    data=b"",
                    method="POST",
                    headers={"traceparent": TRACEPARENT},
                )
                with urllib.request.urlopen(broadcast_request, timeout=5) as response:
                    assert response.status == 202
                opcode, broadcast = read_frame(websocket)
                assert opcode == 1
                assert json.loads(broadcast)["payload"]["source"] == "worker"
                send_masked_frame(websocket, 8, (1000).to_bytes(2, "big"))
            finally:
                websocket.close()

            spans = capture.wait_for_spans(
                {
                    "GET /api/v1/fixture/{fixtureId}",
                    "DmxFixtureService.getFixture",
                    "Database.getFixture",
                    "getFixtureJson.mongoQuery",
                    "GET /api/v1/websocket",
                    "WebSocket.session",
                    "POST /__spike/broadcast",
                    "WebSocket.broadcast",
                }
            )
            assert_trace(spans)
            print("PASS: real Mongo fixture response and exported OTLP parent hierarchy")
    finally:
        if server is not None and server.poll() is None:
            server.send_signal(signal.SIGTERM)
            try:
                server.wait(timeout=5)
            except subprocess.TimeoutExpired:
                server.kill()
        receiver.shutdown()
        receiver.server_close()
        subprocess.run(["docker", "stop", container_name], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


if __name__ == "__main__":
    main()
