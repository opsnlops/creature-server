#!/usr/bin/env python3
"""Production-shaped concurrency gate for the uWebSockets transport spike."""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import urllib.parse
import uuid
from pathlib import Path

import transport_contract_test as contract


SERVER_EXECUTABLE: Path | None = None
SOUNDS_LOCATION: Path | None = None
SOUND_FILE = "dialog-render.wav"


class ServerHarness:
    def __init__(self, sounds_location: Path | None = None) -> None:
        if SERVER_EXECUTABLE is None or SOUNDS_LOCATION is None:
            raise RuntimeError("test configuration was not initialized")
        self.sounds_location = sounds_location or SOUNDS_LOCATION
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0))
            self.port = reservation.getsockname()[1]
        environment = os.environ.copy()
        environment["SERVER_PORT"] = str(self.port)
        environment["SPIKE_FILE_READ_DELAY_MS"] = "250"
        self.process = subprocess.Popen(
            [str(SERVER_EXECUTABLE), "--sounds-location", str(self.sounds_location)],
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        self.config = contract.parse_base_url(f"http://127.0.0.1:{self.port}", False)
        self._wait_until_ready()

    def _close_output(self) -> str:
        if self.process.stdout is None:
            return ""
        output = self.process.stdout.read()
        self.process.stdout.close()
        self.process.stdout = None
        return output

    def _wait_until_ready(self) -> None:
        deadline = time.monotonic() + 5.0
        last_error: BaseException | None = None
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                output = self._close_output()
                raise RuntimeError(f"server exited during startup ({self.process.returncode}): {output}")
            try:
                code, _, _ = contract.http_request(self.config, "GET", "/api/v1/health")
                if code == 200:
                    return
            except (ConnectionError, OSError) as error:
                last_error = error
            time.sleep(0.02)
        raise RuntimeError(f"server did not become ready: {last_error}")

    def stop(self, timeout: float = 2.0) -> float:
        if self.process.poll() is not None:
            return 0.0
        started = time.monotonic()
        self.process.send_signal(signal.SIGTERM)
        try:
            self.process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=2.0)
            output = self._close_output()
            raise AssertionError(f"server did not stop within {timeout:.1f}s: {output}")
        elapsed = time.monotonic() - started
        if self.process.returncode != 0:
            output = self._close_output()
            raise AssertionError(f"server exited with {self.process.returncode}: {output}")
        self._close_output()
        return elapsed

    def force_stop(self) -> None:
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait(timeout=2.0)
        self._close_output()


def open_websocket(config: contract.ContractConfig) -> tuple[socket.socket, contract.HttpSocketReader]:
    test = contract.TransportContractTests(methodName="test_websocket_upgrade_fragmentation_and_ping_pong")
    previous = contract.CONFIG
    contract.CONFIG = config
    try:
        return test.open_websocket()
    finally:
        contract.CONFIG = previous


class UWebSocketsConcurrencyGateTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary_sounds: tempfile.TemporaryDirectory[str] | None = None
        sounds_location = None
        if self._testMethodName == "test_fifo_in_sounds_directory_is_rejected_without_blocking":
            self.temporary_sounds = tempfile.TemporaryDirectory(prefix="creature-uws-fifo-")
            sounds_location = Path(self.temporary_sounds.name)
        self.server = ServerHarness(sounds_location)

    def tearDown(self) -> None:
        self.server.force_stop()
        if self.temporary_sounds is not None:
            self.temporary_sounds.cleanup()

    def test_blocking_work_does_not_stall_health_and_queue_is_bounded(self) -> None:
        start = threading.Event()

        def request_work() -> int:
            start.wait()
            code, _, _ = contract.http_request(self.server.config, "GET", "/__spike/blocking/750")
            return code

        with concurrent.futures.ThreadPoolExecutor(max_workers=32) as callers:
            futures = [callers.submit(request_work) for _ in range(32)]
            start.set()
            time.sleep(0.08)
            health_started = time.monotonic()
            health_code, _, _ = contract.http_request(self.server.config, "GET", "/api/v1/health")
            health_elapsed = time.monotonic() - health_started
            statuses = [future.result(timeout=3.0) for future in futures]

        self.assertEqual(health_code, 200)
        self.assertLess(health_elapsed, 0.2, "blocking service work stalled the uWS loop")
        self.assertIn(200, statuses)
        self.assertIn(503, statuses, "bounded work queue never rejected overload")
        self.assertTrue(set(statuses).issubset({200, 503}))

    def test_client_disconnect_during_work_is_safe(self) -> None:
        connection = contract.open_socket(self.server.config)
        request = (
            "GET /__spike/blocking/300 HTTP/1.1\r\n"
            f"Host: {self.server.config.host_header}\r\n"
            "Connection: close\r\n\r\n"
        ).encode("ascii")
        connection.sendall(request)
        connection.close()
        time.sleep(0.45)
        code, _, _ = contract.http_request(self.server.config, "GET", "/api/v1/health")
        self.assertEqual(code, 200)

    def test_worker_can_broadcast_back_on_the_transport_loop(self) -> None:
        connection, reader = open_websocket(self.server.config)
        try:
            code, _, raw = contract.http_request(self.server.config, "POST", "/__spike/broadcast")
            self.assertEqual(code, 202)
            self.assertEqual(json.loads(raw).get("status"), "accepted")
            _, opcode, payload = contract.read_matching_server_frame(
                connection,
                reader,
                lambda final, candidate_opcode, candidate_payload: final
                and candidate_opcode == 0x1
                and json.loads(candidate_payload).get("command") == "concurrency_gate",
                timeout=2.0,
            )
            self.assertEqual(opcode, 0x1)
            self.assertEqual(json.loads(payload).get("payload", {}).get("source"), "worker")
        finally:
            connection.close()

    def test_slow_websocket_is_closed_at_the_backpressure_limit(self) -> None:
        connection, reader = open_websocket(self.server.config)
        try:
            code, _, _ = contract.http_request(self.server.config, "POST", "/__spike/broadcast/burst")
            self.assertEqual(code, 202)
            health_code, _, _ = contract.http_request(self.server.config, "GET", "/api/v1/health")
            self.assertEqual(health_code, 200)
            self.assertTrue(
                contract.wait_for_websocket_close(connection, reader, timeout=2.0),
                "non-reading WebSocket exceeded the configured backpressure budget without closing",
            )
        finally:
            connection.close()

    def test_many_stalled_invalid_frames_do_not_starve_health_or_shutdown(self) -> None:
        connections: list[socket.socket] = []
        try:
            for _ in range(128):
                connection, _ = open_websocket(self.server.config)
                connections.append(connection)
                connection.sendall(bytes((0x81, 0x7F)) + struct.pack("!Q", 0xFFFFFFFFFFFFFFFF))

            started = time.monotonic()
            health_code, _, _ = contract.http_request(self.server.config, "GET", "/api/v1/health")
            self.assertEqual(health_code, 200)
            self.assertLess(time.monotonic() - started, 0.2, "stalled invalid frames starved the uWS loop")
            self.assertLess(self.server.stop(timeout=2.0), 2.0)
        finally:
            for connection in connections:
                connection.close()

    def test_slow_sound_client_does_not_stall_health(self) -> None:
        connection = contract.open_socket(self.server.config)
        try:
            sound_path = "/api/v1/sound/" + urllib.parse.quote(SOUND_FILE, safe="")
            connection.sendall(
                (
                    f"GET {sound_path} HTTP/1.1\r\n"
                    f"Host: {self.server.config.host_header}\r\n"
                    "Connection: close\r\n\r\n"
                ).encode("ascii")
            )
            response_head = contract.HttpSocketReader(connection).read_until(b"\r\n\r\n")
            self.assertTrue(response_head.startswith(b"HTTP/1.1 200 "))
            time.sleep(0.05)
            started = time.monotonic()
            health_code, _, _ = contract.http_request(self.server.config, "GET", "/api/v1/health")
            elapsed = time.monotonic() - started
            self.assertEqual(health_code, 200)
            self.assertLess(elapsed, 0.2, "a non-reading sound client stalled the uWS loop")
        finally:
            connection.close()

    def test_fifo_in_sounds_directory_is_rejected_without_blocking(self) -> None:
        fifo_name = f"transport-gate-{uuid.uuid4()}.fifo"
        fifo_path = self.server.sounds_location / fifo_name
        os.mkfifo(fifo_path)
        try:
            started = time.monotonic()
            code, _, _ = contract.http_request(
                self.server.config,
                "GET",
                "/api/v1/sound/" + urllib.parse.quote(fifo_name, safe=""),
            )
            self.assertEqual(code, 404)
            self.assertLess(time.monotonic() - started, 0.2, "opening a FIFO blocked the file executor")
            self.assertLess(self.server.stop(timeout=2.0), 2.0)
        finally:
            fifo_path.unlink(missing_ok=True)

    def test_shutdown_closes_active_websocket_and_cancels_worker(self) -> None:
        websocket, _ = open_websocket(self.server.config)
        request = contract.open_socket(self.server.config)
        request.sendall(
            (
                "GET /__spike/blocking/10000 HTTP/1.1\r\n"
                f"Host: {self.server.config.host_header}\r\n"
                "Connection: close\r\n\r\n"
            ).encode("ascii")
        )
        time.sleep(0.05)
        try:
            elapsed = self.server.stop(timeout=2.0)
            self.assertLess(elapsed, 2.0)
        finally:
            websocket.close()
            request.close()


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", required=True, type=Path)
    parser.add_argument("--sounds-location", required=True, type=Path)
    return parser


if __name__ == "__main__":
    arguments, unittest_arguments = make_parser().parse_known_args()
    SERVER_EXECUTABLE = arguments.server.resolve()
    SOUNDS_LOCATION = arguments.sounds_location.resolve()
    if not SERVER_EXECUTABLE.is_file():
        raise SystemExit(f"server executable not found: {SERVER_EXECUTABLE}")
    if not SOUNDS_LOCATION.is_dir():
        raise SystemExit(f"sounds directory not found: {SOUNDS_LOCATION}")
    if not (SOUNDS_LOCATION / SOUND_FILE).is_file():
        raise SystemExit(f"sample sound not found: {SOUNDS_LOCATION / SOUND_FILE}")
    unittest.main(argv=[sys.argv[0], *unittest_arguments], verbosity=2)
