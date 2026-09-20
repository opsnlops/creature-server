#!/usr/bin/env python3
"""Black-box gate for streamed file responses on the uWebSockets transport.

Launches the real ``creature-server`` executable against a temporary sounds
directory and checks that ``GET /api/v1/sound/{filename}`` streams a large WAV
byte-for-byte, that HEAD agrees with GET, that path-safety and not-found
responses keep their oat++ shape, and that a client that aborts or reads
slowly mid-stream never stalls the loop or crashes the server.

Usage:
    python3 tests/transport/uwebsockets_file_stream_gate_test.py \
        --server build/creature-server --network-device lo0
"""

from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import os
import socket
import sys
import time
import urllib.parse
from pathlib import Path

import transport_contract_test as contract
from uwebsockets_production_gate_test import ProductionServer, require

BIG_SOUND_BYTES = 40 * 1024 * 1024
SMALL_SOUND_BYTES = 4096
HEALTH_LATENCY_BUDGET_S = 0.25


def pseudo_random_bytes(size: int, seed: bytes) -> bytes:
    blocks = []
    counter = 0
    produced = 0
    while produced < size:
        block = hashlib.sha256(seed + counter.to_bytes(8, "little")).digest()
        blocks.append(block)
        produced += len(block)
        counter += 1
    return b"".join(blocks)[:size]


def raw_get(config: contract.ContractConfig, path: str, timeout: float = 10.0) -> socket.socket:
    sock = socket.create_connection((config.host, config.port), timeout=timeout)
    sock.sendall(f"GET {path} HTTP/1.1\r\nHost: {config.host}\r\nConnection: close\r\n\r\n".encode())
    return sock


def read_headers(sock: socket.socket) -> tuple[bytes, bytes]:
    buffer = b""
    while b"\r\n\r\n" not in buffer:
        chunk = sock.recv(65536)
        if not chunk:
            raise AssertionError("connection closed before response headers arrived")
        buffer += chunk
    head, rest = buffer.split(b"\r\n\r\n", 1)
    return head, rest


def health_probe(config: contract.ContractConfig) -> float:
    started = time.monotonic()
    code, _, _ = contract.http_request(config, "GET", "/api/v1/health")
    require(code == 200, f"health probe returned {code} while a file stream was active")
    return time.monotonic() - started


def check_full_get_and_head(server: ProductionServer, name: str, payload: bytes) -> None:
    path = "/api/v1/sound/" + urllib.parse.quote(name, safe="")
    code, headers, body = contract.http_request(
        server.config, "GET", path, max_response_bytes=len(payload) + 1
    )
    require(code == 200, f"GET {name} returned {code}: {body[:200]!r}")
    require(headers.get("content-type") == "audio/wav", f"GET {name} content-type {headers.get('content-type')}")
    require(headers.get("content-length") == str(len(payload)), f"GET {name} content-length {headers.get('content-length')}")
    require(len(body) == len(payload), f"GET {name} delivered {len(body)} of {len(payload)} bytes")
    require(hashlib.sha256(body).digest() == hashlib.sha256(payload).digest(), f"GET {name} bytes differ")

    head_code, head_headers, head_body = contract.http_request(server.config, "HEAD", path)
    require(head_code == 200, f"HEAD {name} returned {head_code}")
    require(head_body == b"", f"HEAD {name} carried a body of {len(head_body)} bytes")
    require(head_headers.get("content-length") == str(len(payload)), f"HEAD {name} content-length {head_headers.get('content-length')}")
    require(head_headers.get("content-type") == headers.get("content-type"), f"HEAD {name} content-type differs from GET")


def check_error_shapes(server: ProductionServer) -> None:
    code, headers, body = contract.http_request(server.config, "GET", "/api/v1/sound/does-not-exist.wav")
    require(code == 404, f"missing sound returned {code}")
    require(headers.get("content-type") == "application/json; charset=utf-8", "missing sound is not a JSON envelope")

    traversal = "/api/v1/sound/" + urllib.parse.quote("../etc/passwd", safe="")
    code, headers, body = contract.http_request(server.config, "GET", traversal)
    require(code == 403, f"path traversal returned {code}: {body[:200]!r}")
    require(headers.get("content-type") == "application/json; charset=utf-8", "traversal rejection is not a JSON envelope")

    code, _, body = contract.http_request(server.config, "GET", "/api/v1/sound/bad%20name.wav")
    require(code == 403, f"unsafe filename returned {code}: {body[:200]!r}")

    code, _, body = contract.http_request(server.config, "GET", "/api/v1/sound/provenance/does-not-exist.wav")
    require(code == 404, f"missing provenance returned {code}: {body[:200]!r}")

    code, _, body = contract.http_request(server.config, "GET", "/api/v1/sound/mp3/wrong-extension.wav")
    require(code == 422, f"mp3 rendition with wrong extension returned {code}: {body[:200]!r}")


def check_abort_mid_stream(server: ProductionServer, name: str, payload: bytes) -> None:
    path = "/api/v1/sound/" + urllib.parse.quote(name, safe="")
    sock = raw_get(server.config, path)
    head, rest = read_headers(sock)
    require(head.startswith(b"HTTP/1.1 200"), f"abort test got {head[:40]!r}")
    received = len(rest)
    while received < 1024 * 1024:
        chunk = sock.recv(65536)
        if not chunk:
            raise AssertionError("stream ended before the abort point")
        received += len(chunk)
    sock.close()
    # The loop must notice the disconnect and carry on serving.
    for _ in range(5):
        require(health_probe(server.config) < HEALTH_LATENCY_BUDGET_S, "health slowed down after a mid-stream abort")
    check_full_get_and_head(server, name, payload)


def check_slow_reader_keeps_loop_responsive(server: ProductionServer, name: str, payload: bytes) -> None:
    path = "/api/v1/sound/" + urllib.parse.quote(name, safe="")
    sock = raw_get(server.config, path, timeout=30.0)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 32 * 1024)
    head, rest = read_headers(sock)
    require(head.startswith(b"HTTP/1.1 200"), f"slow reader got {head[:40]!r}")
    digest = hashlib.sha256(rest)
    received = len(rest)
    worst = 0.0
    probes = 0
    started = time.monotonic()
    while received < len(payload):
        chunk = sock.recv(16 * 1024)
        if not chunk:
            raise AssertionError(f"slow reader stream ended at {received} of {len(payload)} bytes")
        digest.update(chunk)
        received += len(chunk)
        if received < 8 * 1024 * 1024:
            time.sleep(0.001)  # hold the socket back so the server hits backpressure
        if probes < 20 and time.monotonic() - started > probes * 0.1:
            worst = max(worst, health_probe(server.config))
            probes += 1
    sock.close()
    require(received == len(payload), f"slow reader received {received} of {len(payload)} bytes")
    require(digest.digest() == hashlib.sha256(payload).digest(), "slow reader bytes differ")
    require(probes > 0, "slow reader finished before any health probe ran")
    require(worst < HEALTH_LATENCY_BUDGET_S, f"health took {worst:.3f}s during a backpressured stream")


def check_parallel_streams(server: ProductionServer, name: str, payload: bytes) -> None:
    path = "/api/v1/sound/" + urllib.parse.quote(name, safe="")
    expected = hashlib.sha256(payload).digest()

    def fetch(index: int) -> tuple[int, int, bool]:
        # Stagger the starts: resolving the file is application work with a
        # small admission queue, while the streams themselves live on the loop
        # and the file executor. A burst of 40 identical instants would be a
        # test of application-queue saturation (a deliberate 503), not of
        # concurrent streaming.
        time.sleep(0.02 * index)
        code, _, body = contract.http_request(server.config, "GET", path, max_response_bytes=len(payload) + 1)
        return code, len(body), hashlib.sha256(body).digest() == expected

    # More concurrent streams than the file executor has workers, well past
    # the point where a fixed read-queue bound would have torn one down.
    with concurrent.futures.ThreadPoolExecutor(max_workers=40) as pool:
        results = list(pool.map(fetch, range(40)))
    rejected = [r for r in results if r[0] == 503]
    require(not rejected, f"{len(rejected)} of 40 staggered streams were rejected by application admission")
    wrong = [r for r in results if r[0] != 200 or not r[2]]
    require(not wrong, f"{len(wrong)} of 40 parallel streams were not byte-exact: {wrong[:5]!r}")


def server_rss_bytes(server: ProductionServer) -> int:
    import subprocess

    output = subprocess.run(["ps", "-o", "rss=", "-p", str(server.process.pid)], capture_output=True, text=True)
    return int(output.stdout.strip() or "0") * 1024


def check_advertised_length_is_not_trusted(server: ProductionServer) -> None:
    """Announcing a route's full body limit must not reserve it up front.

    Forty idle uploads each claiming 1 GiB would be 40 GiB of reservations if
    the loop trusted Content-Length; the transport caps the reserve, so RSS
    barely moves and health keeps answering while the sockets sit open.
    """
    before = server_rss_bytes(server)
    sockets = []
    try:
        for _ in range(40):
            sock = socket.create_connection((server.config.host, server.config.port), timeout=10.0)
            sock.sendall(
                (
                    "POST /api/v1/sound/generate-lipsync/upload?filename=take.wav HTTP/1.1\r\n"
                    f"Host: {server.config.host}\r\n"
                    "Content-Type: audio/wav\r\n"
                    "Content-Length: 1073741824\r\n\r\n"
                ).encode()
            )
            sockets.append(sock)
        time.sleep(0.2)
        for _ in range(3):
            require(health_probe(server.config) < HEALTH_LATENCY_BUDGET_S, "health slowed down under idle uploads")
        after = server_rss_bytes(server)
        require(after - before < 256 * 1024 * 1024, f"RSS grew by {(after - before) // (1024 * 1024)} MiB with no body sent")
    finally:
        for sock in sockets:
            sock.close()
    require(health_probe(server.config) < HEALTH_LATENCY_BUDGET_S, "health slowed down after idle uploads closed")


def check_fifo_does_not_stall(server: ProductionServer) -> None:
    fifo = Path(server.sounds.name) / "trap.wav"
    os.mkfifo(fifo)
    try:
        started = time.monotonic()
        code, _, body = contract.http_request(server.config, "GET", "/api/v1/sound/trap.wav")
        elapsed = time.monotonic() - started
        require(code in {404, 500}, f"FIFO sound returned {code}: {body[:200]!r}")
        require(elapsed < 2.0, f"FIFO sound took {elapsed:.2f}s to reject")
        require(health_probe(server.config) < HEALTH_LATENCY_BUDGET_S, "health slowed down after a FIFO request")
    finally:
        fifo.unlink()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--server", required=True, type=Path)
    parser.add_argument("--network-device", required=True, help="loopback network interface name (for example lo0 or lo)")
    arguments = parser.parse_args()
    executable = arguments.server.resolve()
    if not executable.is_file():
        parser.error(f"server executable not found: {executable}")

    server = ProductionServer(executable, arguments.network_device, "uwebsockets")
    try:
        big = pseudo_random_bytes(BIG_SOUND_BYTES, b"creature-file-stream-gate")
        small = pseudo_random_bytes(SMALL_SOUND_BYTES, b"creature-file-stream-small")
        (Path(server.sounds.name) / "big-take.wav").write_bytes(big)
        (Path(server.sounds.name) / "small-take.wav").write_bytes(small)
        empty = Path(server.sounds.name) / "empty-take.wav"
        empty.write_bytes(b"")

        started = time.monotonic()
        check_full_get_and_head(server, "small-take.wav", small)
        check_full_get_and_head(server, "empty-take.wav", b"")
        check_full_get_and_head(server, "big-take.wav", big)
        check_error_shapes(server)
        check_abort_mid_stream(server, "big-take.wav", big)
        check_slow_reader_keeps_loop_responsive(server, "big-take.wav", big)
        check_parallel_streams(server, "big-take.wav", big)
        check_advertised_length_is_not_trusted(server)
        check_fifo_does_not_stall(server)
        elapsed = time.monotonic() - started
        stop_seconds = server.stop()
        print(
            f"PASS: streamed {BIG_SOUND_BYTES // (1024 * 1024)} MiB byte-exact with HEAD/GET agreement, error "
            f"envelopes, mid-stream abort, backpressured slow reader, 40 staggered parallel streams, untrusted Content-Length, and FIFO rejection "
            f"({elapsed:.1f}s; shutdown {stop_seconds:.3f}s)"
        )
        return 0
    except AssertionError as error:
        print(f"FAIL: {error}", file=sys.stderr)
        print(server.logs(), file=sys.stderr)
        return 1
    finally:
        server.close()


if __name__ == "__main__":
    sys.exit(main())
