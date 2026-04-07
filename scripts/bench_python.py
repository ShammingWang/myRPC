#!/usr/bin/env python3
import argparse
import socket
import statistics
import struct
import threading
import time
from dataclasses import dataclass, field
from typing import List, Optional


MAGIC = 0x4D525043  # MRPC
VERSION = 1
REQUEST_TYPE = 1
RESPONSE_TYPE = 2
HEADER_STRUCT = struct.Struct("!IBBHIIQ")
HEADER_SIZE = HEADER_STRUCT.size


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="myRPC Python benchmark client",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("--host", default="127.0.0.1", help="server host")
    parser.add_argument("--port", type=int, default=8080, help="server port")
    parser.add_argument("--method", default="echo", help="RPC method name")
    parser.add_argument("--payload", default="hello rpc", help="request payload")
    parser.add_argument(
        "--connections", type=int, default=100, help="number of concurrent client connections"
    )
    parser.add_argument(
        "--requests",
        type=int,
        default=10000,
        help="total request count; ignored when --duration is set",
    )
    parser.add_argument(
        "--duration",
        type=float,
        default=0.0,
        help="benchmark duration in seconds; when > 0, runs until timeout instead of request count",
    )
    parser.add_argument("--timeout", type=float, default=5.0, help="socket timeout in seconds")
    parser.add_argument(
        "--expect-status",
        type=int,
        default=0,
        help="expected response status code",
    )
    parser.add_argument(
        "--expect-payload",
        default=None,
        help="optional expected response payload for validation",
    )
    parser.add_argument(
        "--reconnect-per-request",
        action="store_true",
        help="create a fresh TCP connection for every request instead of connection reuse",
    )
    args = parser.parse_args()

    if args.connections <= 0:
        parser.error("--connections must be > 0")
    if args.requests <= 0 and args.duration <= 0:
        parser.error("--requests must be > 0 when --duration is not set")
    if args.port <= 0 or args.port > 65535:
        parser.error("--port must be in 1..65535")
    if args.timeout <= 0:
        parser.error("--timeout must be > 0")

    return args


def build_request(request_id: int, method: bytes, payload: bytes) -> bytes:
    header = HEADER_STRUCT.pack(
        MAGIC,
        VERSION,
        REQUEST_TYPE,
        0,
        len(method),
        len(payload),
        request_id,
    )
    return header + method + payload


def recv_exact(sock: socket.socket, length: int) -> bytes:
    chunks = bytearray()
    while len(chunks) < length:
        chunk = sock.recv(length - len(chunks))
        if not chunk:
            raise ConnectionError("connection closed by peer")
        chunks.extend(chunk)
    return bytes(chunks)


def read_response(sock: socket.socket) -> tuple[int, int, bytes]:
    header = recv_exact(sock, HEADER_SIZE)
    magic, version, msg_type, status, method_len, body_len, request_id = HEADER_STRUCT.unpack(
        header
    )
    if magic != MAGIC:
        raise ValueError(f"invalid magic: 0x{magic:08x}")
    if version != VERSION:
        raise ValueError(f"unsupported version: {version}")
    if msg_type != RESPONSE_TYPE:
        raise ValueError(f"unexpected message type: {msg_type}")
    if method_len != 0:
        raise ValueError(f"unexpected response method length: {method_len}")
    body = recv_exact(sock, body_len)
    return request_id, status, body


@dataclass
class SharedState:
    total_requests: int
    duration: float
    start_time: float = field(default_factory=time.perf_counter)
    latencies_ms: List[float] = field(default_factory=list)
    success: int = 0
    failures: int = 0
    bytes_sent: int = 0
    bytes_received: int = 0
    next_request_number: int = 0
    stop: bool = False
    first_error: Optional[str] = None
    lock: threading.Lock = field(default_factory=threading.Lock)

    def acquire_request_index(self) -> Optional[int]:
        with self.lock:
            if self.stop:
                return None
            if self.duration > 0:
                if time.perf_counter() - self.start_time >= self.duration:
                    self.stop = True
                    return None
            elif self.next_request_number >= self.total_requests:
                return None

            request_number = self.next_request_number
            self.next_request_number += 1
            return request_number

    def record_success(self, latency_ms: float, sent_bytes: int, recv_bytes: int) -> None:
        with self.lock:
            self.success += 1
            self.bytes_sent += sent_bytes
            self.bytes_received += recv_bytes
            self.latencies_ms.append(latency_ms)
            if self.duration <= 0 and self.success >= self.total_requests:
                self.stop = True

    def record_failure(self, message: str) -> None:
        with self.lock:
            self.failures += 1
            self.stop = True
            if self.first_error is None:
                self.first_error = message


class Worker:
    def __init__(self, worker_id: int, args: argparse.Namespace, state: SharedState) -> None:
        self.worker_id = worker_id
        self.args = args
        self.state = state
        self.method = args.method.encode("utf-8")
        self.payload = args.payload.encode("utf-8")
        self.expect_payload = None
        if args.expect_payload is not None:
            self.expect_payload = args.expect_payload.encode("utf-8")
        self.sock: Optional[socket.socket] = None

    def connect(self) -> socket.socket:
        sock = socket.create_connection((self.args.host, self.args.port), timeout=self.args.timeout)
        sock.settimeout(self.args.timeout)
        return sock

    def ensure_connection(self) -> socket.socket:
        if self.sock is None:
            self.sock = self.connect()
        return self.sock

    def close(self) -> None:
        if self.sock is not None:
            try:
                self.sock.close()
            finally:
                self.sock = None

    def run(self) -> None:
        try:
            while True:
                request_number = self.state.acquire_request_index()
                if request_number is None:
                    return

                request_id = (self.worker_id << 48) | request_number
                packet = build_request(request_id, self.method, self.payload)
                start = time.perf_counter()

                try:
                    if self.args.reconnect_per_request:
                        with self.connect() as sock:
                            sock.sendall(packet)
                            response_request_id, status, body = read_response(sock)
                    else:
                        sock = self.ensure_connection()
                        sock.sendall(packet)
                        response_request_id, status, body = read_response(sock)
                except Exception as exc:  # noqa: BLE001
                    self.state.record_failure(f"worker {self.worker_id} request failed: {exc}")
                    return

                latency_ms = (time.perf_counter() - start) * 1000.0
                if response_request_id != request_id:
                    self.state.record_failure(
                        f"worker {self.worker_id} request_id mismatch: sent={request_id} recv={response_request_id}"
                    )
                    return
                if status != self.args.expect_status:
                    self.state.record_failure(
                        f"worker {self.worker_id} unexpected status: {status}, expected={self.args.expect_status}"
                    )
                    return
                if self.expect_payload is not None and body != self.expect_payload:
                    self.state.record_failure(
                        f"worker {self.worker_id} unexpected payload: {body!r}, expected={self.expect_payload!r}"
                    )
                    return

                self.state.record_success(latency_ms, len(packet), HEADER_SIZE + len(body))
        finally:
            self.close()


def percentile(sorted_values: List[float], ratio: float) -> float:
    if not sorted_values:
        return 0.0
    if len(sorted_values) == 1:
        return sorted_values[0]
    position = ratio * (len(sorted_values) - 1)
    lower = int(position)
    upper = min(lower + 1, len(sorted_values) - 1)
    weight = position - lower
    return sorted_values[lower] * (1.0 - weight) + sorted_values[upper] * weight


def print_report(args: argparse.Namespace, state: SharedState) -> int:
    elapsed = max(time.perf_counter() - state.start_time, 1e-9)
    latencies = sorted(state.latencies_ms)
    total_completed = state.success + state.failures

    print("Benchmark finished")
    print(f"Target       : {args.host}:{args.port}")
    print(f"Method       : {args.method}")
    print(f"Payload size : {len(args.payload.encode('utf-8'))} bytes")
    print(f"Connections  : {args.connections}")
    print(f"Elapsed      : {elapsed:.3f} s")
    print(f"Completed    : {total_completed}")
    print(f"Success      : {state.success}")
    print(f"Failures     : {state.failures}")
    print(f"QPS          : {state.success / elapsed:.2f}")
    print(f"Tx throughput: {state.bytes_sent / elapsed / 1024:.2f} KiB/s")
    print(f"Rx throughput: {state.bytes_received / elapsed / 1024:.2f} KiB/s")

    if latencies:
        print(f"Latency avg  : {statistics.fmean(latencies):.3f} ms")
        print(f"Latency min  : {latencies[0]:.3f} ms")
        print(f"Latency p50  : {percentile(latencies, 0.50):.3f} ms")
        print(f"Latency p90  : {percentile(latencies, 0.90):.3f} ms")
        print(f"Latency p99  : {percentile(latencies, 0.99):.3f} ms")
        print(f"Latency max  : {latencies[-1]:.3f} ms")

    if state.first_error:
        print(f"First error  : {state.first_error}")

    return 0 if state.failures == 0 else 1


def main() -> int:
    args = parse_args()
    state = SharedState(total_requests=args.requests, duration=args.duration)

    threads = []
    for worker_id in range(args.connections):
        worker = Worker(worker_id + 1, args, state)
        thread = threading.Thread(target=worker.run, name=f"bench-worker-{worker_id + 1}")
        thread.start()
        threads.append(thread)

    for thread in threads:
        thread.join()

    return print_report(args, state)


if __name__ == "__main__":
    raise SystemExit(main())
