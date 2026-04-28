#!/usr/bin/env python3
import argparse
import csv
import datetime as dt
import json
import os
import platform
import signal
import socket
import statistics
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import Any


ROOT_DIR = Path(__file__).resolve().parent.parent
BUILD_DIR = ROOT_DIR / "build"
DEFAULT_SERVER = BUILD_DIR / "simple_tcp_server"
DEFAULT_CLIENT = BUILD_DIR / "mrpc_bench_client"
DEFAULT_RESULTS_DIR = ROOT_DIR / "bench" / "results"
SUMMARY_METRICS = [
    "mrpc_connections_active",
    "mrpc_requests_inflight",
    "mrpc_completed_requests_average_qps",
    "mrpc_latency_queue_average_ms",
    "mrpc_latency_queue_max_ms",
    "mrpc_latency_handler_average_ms",
    "mrpc_latency_handler_max_ms",
    "mrpc_latency_return_queue_average_ms",
    "mrpc_latency_return_queue_max_ms",
    "mrpc_latency_send_average_ms",
    "mrpc_latency_send_max_ms",
    "mrpc_latency_io_return_average_ms",
    "mrpc_latency_io_return_max_ms",
    "mrpc_latency_end_to_end_average_ms",
    "mrpc_latency_end_to_end_max_ms",
    "mrpc_worker_rejections_total",
    "mrpc_protocol_errors_total",
]
SUMMARY_FIELDS = [
    "qps_mean",
    "qps_best",
    "latency_avg_ms_mean",
    "latency_p50_ms_mean",
    "latency_p90_ms_mean",
    "latency_p99_ms_mean",
    "latency_p999_ms_mean",
    "tx_kib_per_s_mean",
    "rx_kib_per_s_mean",
]


def parse_int_list(value: str) -> list[int]:
    items = [item.strip() for item in value.split(",") if item.strip()]
    if not items:
        raise argparse.ArgumentTypeError("expected a non-empty comma-separated integer list")
    try:
        return [int(item) for item in items]
    except ValueError as exc:
        raise argparse.ArgumentTypeError(str(exc)) from exc


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run the myRPC benchmark suite and generate repeatable reports.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("--host", default="127.0.0.1", help="server host")
    parser.add_argument("--port", type=int, default=8080, help="server port")
    parser.add_argument("--admin-port", type=int, default=9090, help="server admin port")
    parser.add_argument(
        "--client",
        default=str(DEFAULT_CLIENT),
        help="path to benchmark client executable",
    )
    parser.add_argument(
        "--server",
        default=str(DEFAULT_SERVER),
        help="path to server executable",
    )
    parser.add_argument(
        "--duration", type=float, default=6.0, help="benchmark duration for each measured run"
    )
    parser.add_argument("--warmup", type=float, default=2.0, help="warmup duration per case")
    parser.add_argument("--repeats", type=int, default=3, help="number of measured repeats")
    parser.add_argument(
        "--connections",
        type=parse_int_list,
        default=parse_int_list("50,100,200,400"),
        help="connection counts for the throughput sweep",
    )
    parser.add_argument(
        "--io-threads",
        type=parse_int_list,
        default=parse_int_list("1,2,4,8,16"),
        help="io thread counts for the thread scaling sweep",
    )
    parser.add_argument(
        "--payload-sizes",
        type=parse_int_list,
        default=parse_int_list("16,256,4096,16384,65536"),
        help="payload sizes in bytes for the payload sweep",
    )
    parser.add_argument(
        "--outstanding-per-connection",
        type=parse_int_list,
        default=parse_int_list("1,4,16,64"),
        help="per-connection pipelined request depths for the single-connection multiplexing sweep",
    )
    parser.add_argument(
        "--baseline-connections",
        type=int,
        default=200,
        help="connections for the thread and payload sweeps",
    )
    parser.add_argument(
        "--baseline-io-threads",
        type=int,
        default=max(1, min(os.cpu_count() or 1, 8)),
        help="default io thread count for non-thread-scaling cases",
    )
    parser.add_argument(
        "--method",
        default="EchoService.Echo",
        help="rpc method used by the benchmark suite",
    )
    parser.add_argument(
        "--label",
        default="",
        help="optional label appended to the output directory name",
    )
    parser.add_argument(
        "--output-dir",
        default="",
        help="optional explicit output directory; overrides the auto-generated name",
    )
    parser.add_argument(
        "--compare-with",
        default="",
        help="path to a previous summary CSV produced by this script",
    )
    parser.add_argument(
        "--keep-server-logs",
        action="store_true",
        help="keep per-case server logs instead of only storing them on failure",
    )
    parser.add_argument(
        "--skip-build-check",
        action="store_true",
        help="skip checking whether the benchmark binaries exist",
    )
    parser.add_argument(
        "--metrics-sample-interval",
        type=float,
        default=0.5,
        help="sample /metrics during warmup and measured runs every N seconds; set to 0 to disable",
    )
    args = parser.parse_args()

    if args.port <= 0 or args.port > 65535:
        parser.error("--port must be in 1..65535")
    if args.admin_port < 0 or args.admin_port > 65535:
        parser.error("--admin-port must be in 0..65535")
    if args.duration <= 0:
        parser.error("--duration must be > 0")
    if args.warmup < 0:
        parser.error("--warmup must be >= 0")
    if args.repeats <= 0:
        parser.error("--repeats must be > 0")
    if args.baseline_connections <= 0:
        parser.error("--baseline-connections must be > 0")
    if args.baseline_io_threads <= 0:
        parser.error("--baseline-io-threads must be > 0")
    if any(value <= 0 for value in args.outstanding_per_connection):
        parser.error("--outstanding-per-connection values must be > 0")
    if args.metrics_sample_interval < 0:
        parser.error("--metrics-sample-interval must be >= 0")
    return args


def ensure_binary(path: Path, name: str) -> None:
    if not path.exists():
        raise FileNotFoundError(f"{name} binary not found: {path}")
    if not os.access(path, os.X_OK):
        raise PermissionError(f"{name} binary is not executable: {path}")


def wait_for_port(host: str, port: int, timeout: float) -> None:
    deadline = time.time() + timeout
    while time.time() < deadline:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
            sock.settimeout(0.25)
            try:
                sock.connect((host, port))
                return
            except OSError:
                time.sleep(0.1)
    raise TimeoutError(f"timed out waiting for {host}:{port} to accept connections")


def http_get(host: str, port: int, path: str, timeout: float = 2.0) -> str:
    request = (
        f"GET {path} HTTP/1.1\r\nHost: {host}:{port}\r\nConnection: close\r\n\r\n"
    ).encode("ascii")
    with socket.create_connection((host, port), timeout=timeout) as sock:
        sock.settimeout(timeout)
        sock.sendall(request)
        response = bytearray()
        while True:
            chunk = sock.recv(4096)
            if not chunk:
                break
            response.extend(chunk)
    header_bytes, _, body = bytes(response).partition(b"\r\n\r\n")
    status_line = header_bytes.splitlines()[0].decode("ascii", errors="replace")
    if " 200 " not in status_line:
        raise RuntimeError(f"unexpected admin response status: {status_line}")
    return body.decode("utf-8")


def parse_labels(raw: str) -> dict[str, str]:
    labels: dict[str, str] = {}
    if not raw:
        return labels
    for item in raw.split(","):
        key, value = item.split("=", 1)
        value = value.strip()
        if value.startswith('"') and value.endswith('"'):
            value = value[1:-1]
        value = value.replace('\\"', '"').replace("\\\\", "\\").replace("\\n", "\n")
        labels[key.strip()] = value
    return labels


def parse_metrics_text(text: str) -> tuple[dict[str, float], list[dict[str, Any]]]:
    scalars: dict[str, float] = {}
    labeled: list[dict[str, Any]] = []
    for raw_line in text.splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        metric_with_labels, value_text = line.split(None, 1)
        if "{" in metric_with_labels:
            metric_name, raw_labels = metric_with_labels.split("{", 1)
            labels = parse_labels(raw_labels.rstrip("}"))
            labeled.append(
                {
                    "metric_name": metric_name,
                    "labels": labels,
                    "value": float(value_text),
                }
            )
        else:
            scalars[metric_with_labels] = float(value_text)
    return scalars, labeled


class MetricsSampler:
    def __init__(self, host: str, port: int, interval_seconds: float) -> None:
        self.host = host
        self.port = port
        self.interval_seconds = interval_seconds
        self.stop_event = threading.Event()
        self.state_lock = threading.Lock()
        self.stage = "idle"
        self.repeat = 0
        self.case_id = ""
        self.case_kind = ""
        self.samples: list[dict[str, Any]] = []
        self.errors: list[str] = []
        self.thread = threading.Thread(target=self._run, name="metrics-sampler", daemon=True)

    def start(self) -> None:
        self.thread.start()

    def switch_case(self, case_id: str, case_kind: str) -> None:
        with self.state_lock:
            self.case_id = case_id
            self.case_kind = case_kind
            self.stage = "idle"
            self.repeat = 0

    def set_stage(self, stage: str, repeat: int) -> None:
        with self.state_lock:
            self.stage = stage
            self.repeat = repeat

    def stop(self) -> None:
        self.stop_event.set()
        self.thread.join(timeout=max(1.0, self.interval_seconds + 1.0))

    def _snapshot_state(self) -> tuple[str, str, str, int]:
        with self.state_lock:
            return self.case_id, self.case_kind, self.stage, self.repeat

    def _run(self) -> None:
        while not self.stop_event.wait(self.interval_seconds):
            case_id, case_kind, stage, repeat = self._snapshot_state()
            if not case_id or stage == "idle":
                continue
            try:
                metrics_text = http_get(self.host, self.port, "/metrics")
                scalars, labeled = parse_metrics_text(metrics_text)
                self.samples.append(
                    {
                        "sample_timestamp": dt.datetime.now().isoformat(timespec="milliseconds"),
                        "case_id": case_id,
                        "case_kind": case_kind,
                        "stage": stage,
                        "repeat": repeat,
                        "scalars": scalars,
                        "labeled": labeled,
                    }
                )
            except Exception as exc:  # noqa: BLE001
                self.errors.append(
                    f"{dt.datetime.now().isoformat(timespec='seconds')} {case_id} {stage}#{repeat}: {exc}"
                )


def build_payload(size: int) -> str:
    alphabet = "0123456789abcdefghijklmnopqrstuvwxyz"
    repeated = (alphabet * ((size // len(alphabet)) + 1))[:size]
    return repeated


def format_ms(value: float) -> str:
    return f"{value:.3f} ms"


def format_qps(value: float) -> str:
    return f"{value:.2f}"


def format_percent(delta: float) -> str:
    return f"{delta:+.1f}%"


def machine_summary() -> str:
    return (
        f"{platform.system()} {platform.release()}, "
        f"{platform.machine()}, cpu_count={os.cpu_count() or 'unknown'}"
    )


def current_commit() -> str:
    try:
        result = subprocess.run(
            ["git", "rev-parse", "--short", "HEAD"],
            cwd=ROOT_DIR,
            check=True,
            capture_output=True,
            text=True,
        )
    except subprocess.CalledProcessError:
        return "unknown"
    return result.stdout.strip() or "unknown"


def sanitize_name(value: str) -> str:
    safe = []
    for ch in value:
        if ch.isalnum() or ch in {"-", "_"}:
            safe.append(ch)
        else:
            safe.append("_")
    return "".join(safe).strip("_") or "benchmark"


def case_id(
    kind: str,
    io_threads: int,
    connections: int,
    payload_size: int,
    outstanding_per_connection: int,
    reconnect_per_request: bool,
    method: str,
) -> str:
    reconnect = "reconnect" if reconnect_per_request else "reuse"
    method_name = sanitize_name(method)
    return (
        f"{kind}__io{io_threads}__conn{connections}__payload{payload_size}"
        f"__out{outstanding_per_connection}__{reconnect}__{method_name}"
    )


def launch_server(
    server_path: Path,
    host: str,
    port: int,
    admin_port: int,
    io_threads: int,
    log_path: Path,
) -> subprocess.Popen[str]:
    env = os.environ.copy()
    env["MRPC_IO_THREADS"] = str(io_threads)
    env["MRPC_ADMIN_PORT"] = str(admin_port)
    env.setdefault("MRPC_ENABLE_REQUEST_TRACE", "false")
    env.setdefault("MRPC_TRACE_ALL_REQUESTS", "false")
    env.setdefault("MRPC_SLOW_REQUEST_MS", "100000")

    log_file = log_path.open("w", encoding="utf-8")
    process = subprocess.Popen(
        [str(server_path)],
        cwd=ROOT_DIR,
        env=env,
        stdout=log_file,
        stderr=subprocess.STDOUT,
        text=True,
        preexec_fn=os.setsid,
    )
    try:
        wait_for_port(host, port, timeout=10.0)
        if admin_port != 0:
            wait_for_port(host, admin_port, timeout=10.0)
    except Exception:
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
        log_file.close()
        raise
    process._bench_log_file = log_file  # type: ignore[attr-defined]
    return process


def stop_server(process: subprocess.Popen[str]) -> None:
    log_file = getattr(process, "_bench_log_file", None)
    try:
        os.killpg(os.getpgid(process.pid), signal.SIGTERM)
    except ProcessLookupError:
        pass
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(os.getpgid(process.pid), signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait(timeout=5)
    if log_file is not None:
        log_file.close()


def run_client(
    client_path: Path,
    host: str,
    port: int,
    method: str,
    connections: int,
    duration: float,
    payload: str,
    outstanding_per_connection: int,
    reconnect_per_request: bool,
) -> dict[str, Any]:
    command = [
        str(client_path),
        "--host",
        host,
        "--port",
        str(port),
        "--method",
        method,
        "--connections",
        str(connections),
        "--duration",
        f"{duration:.3f}",
        "--payload",
        payload,
        "--expect-payload",
        payload,
        "--outstanding-per-connection",
        str(outstanding_per_connection),
        "--report-format",
        "json",
    ]
    if reconnect_per_request:
        command.append("--reconnect-per-request")
    result = subprocess.run(
        command,
        cwd=ROOT_DIR,
        check=False,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        raise RuntimeError(
            f"benchmark client failed with exit code {result.returncode}:\n"
            f"stdout:\n{result.stdout}\n"
            f"stderr:\n{result.stderr}"
        )
    try:
        return json.loads(result.stdout)
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"failed to parse benchmark client JSON:\n{result.stdout}") from exc


def summarize_runs(runs: list[dict[str, Any]]) -> dict[str, Any]:
    def values(path: tuple[str, ...]) -> list[float]:
        current = []
        for run in runs:
            value: Any = run
            for key in path:
                value = value[key]
            current.append(float(value))
        return current

    summary = {
        "run_count": len(runs),
        "success": int(statistics.mean(values(("success",)))),
        "failures": int(statistics.mean(values(("failures",)))),
        "qps_mean": statistics.fmean(values(("qps",))),
        "qps_best": max(values(("qps",))),
        "latency_avg_ms_mean": statistics.fmean(values(("latency_ms", "avg"))),
        "latency_p50_ms_mean": statistics.fmean(values(("latency_ms", "p50"))),
        "latency_p90_ms_mean": statistics.fmean(values(("latency_ms", "p90"))),
        "latency_p99_ms_mean": statistics.fmean(values(("latency_ms", "p99"))),
        "latency_p999_ms_mean": statistics.fmean(values(("latency_ms", "p999"))),
        "latency_max_ms_best": min(values(("latency_ms", "max"))),
        "tx_kib_per_s_mean": statistics.fmean(values(("tx_kib_per_s",))),
        "rx_kib_per_s_mean": statistics.fmean(values(("rx_kib_per_s",))),
        "elapsed_seconds_mean": statistics.fmean(values(("elapsed_seconds",))),
    }
    return summary


def summarize_metric_snapshots(metric_snapshots: list[dict[str, Any]]) -> dict[str, float]:
    if not metric_snapshots:
        return {}
    summary: dict[str, float] = {}
    for metric_name in SUMMARY_METRICS:
        values = [
            float(snapshot["scalars"][metric_name])
            for snapshot in metric_snapshots
            if metric_name in snapshot["scalars"]
        ]
        if values:
            summary[f"{metric_name}_mean"] = statistics.fmean(values)
            summary[f"{metric_name}_max"] = max(values)
    return summary


def load_previous_summary(csv_path: Path) -> dict[str, dict[str, str]]:
    previous: dict[str, dict[str, str]] = {}
    with csv_path.open("r", encoding="utf-8", newline="") as handle:
        for row in csv.DictReader(handle):
            previous[row["case_id"]] = row
    return previous


def write_json(path: Path, data: Any) -> None:
    path.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def write_csv(path: Path, rows: list[dict[str, Any]]) -> None:
    if not rows:
        return
    fieldnames: list[str] = []
    seen: set[str] = set()
    for row in rows:
        for key in row.keys():
            if key not in seen:
                seen.add(key)
                fieldnames.append(key)
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def build_measurement_rows(raw_runs: list[dict[str, Any]]) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for run in raw_runs:
        row: dict[str, Any] = {
            "case_id": run["case_id"],
            "case_kind": run["case_kind"],
            "repeat": run["repeat"],
            "method": run["method"],
            "io_threads": run["io_threads"],
            "connections": run["connections"],
            "payload_size_bytes": run["payload_size_bytes"],
            "outstanding_per_connection": run["outstanding_per_connection"],
            "reconnect_per_request": run["reconnect_per_request"],
            "qps": run["qps"],
            "latency_avg_ms": run["latency_ms"]["avg"],
            "latency_p50_ms": run["latency_ms"]["p50"],
            "latency_p90_ms": run["latency_ms"]["p90"],
            "latency_p99_ms": run["latency_ms"]["p99"],
            "latency_p999_ms": run["latency_ms"]["p999"],
            "latency_max_ms": run["latency_ms"]["max"],
            "tx_kib_per_s": run["tx_kib_per_s"],
            "rx_kib_per_s": run["rx_kib_per_s"],
        }
        for metric_name in SUMMARY_METRICS:
            row[metric_name] = run.get("metrics_scalars", {}).get(metric_name, "")
        rows.append(row)
    return rows


def build_method_metric_rows(metric_snapshots: list[dict[str, Any]]) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for snapshot in metric_snapshots:
        for metric in snapshot["labeled"]:
            method = metric["labels"].get("method")
            if method is None:
                continue
            rows.append(
                {
                    "case_id": snapshot["case_id"],
                    "case_kind": snapshot["case_kind"],
                    "stage": snapshot["stage"],
                    "repeat": snapshot["repeat"],
                    "metric_name": metric["metric_name"],
                    "method": method,
                    "value": metric["value"],
                }
            )
    return rows


def build_metrics_timeseries_rows(metric_snapshots: list[dict[str, Any]]) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for snapshot in metric_snapshots:
        timestamp = snapshot.get("sample_timestamp", "")
        for metric_name, value in snapshot["scalars"].items():
            rows.append(
                {
                    "sample_timestamp": timestamp,
                    "case_id": snapshot["case_id"],
                    "case_kind": snapshot["case_kind"],
                    "stage": snapshot["stage"],
                    "repeat": snapshot["repeat"],
                    "metric_name": metric_name,
                    "value": value,
                }
            )
        for metric in snapshot["labeled"]:
            rows.append(
                {
                    "sample_timestamp": timestamp,
                    "case_id": snapshot["case_id"],
                    "case_kind": snapshot["case_kind"],
                    "stage": snapshot["stage"],
                    "repeat": snapshot["repeat"],
                    "metric_name": metric["metric_name"],
                    "value": metric["value"],
                    **{f"label_{key}": label_value for key, label_value in metric["labels"].items()},
                }
            )
    return rows


def build_plot_rows(summary_rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for row in summary_rows:
        x_axis = (
            "connections"
            if row["case_kind"] == "throughput"
            else "io_threads"
            if row["case_kind"] == "thread_scale"
            else "outstanding_per_connection"
            if row["case_kind"] == "multiplex"
            else "payload_size_bytes"
        )
        x_value = (
            row["connections"]
            if x_axis == "connections"
            else row["io_threads"]
            if x_axis == "io_threads"
            else row["outstanding_per_connection"]
            if x_axis == "outstanding_per_connection"
            else row["payload_size_bytes"]
        )
        for metric_name in SUMMARY_FIELDS:
            rows.append(
                {
                    "case_id": row["case_id"],
                    "case_kind": row["case_kind"],
                    "x_axis": x_axis,
                    "x_value": x_value,
                    "metric_name": metric_name,
                    "value": row[metric_name],
                }
            )
        for metric_name in SUMMARY_METRICS:
            mean_key = f"{metric_name}_mean"
            if mean_key in row:
                rows.append(
                    {
                        "case_id": row["case_id"],
                        "case_kind": row["case_kind"],
                        "x_axis": x_axis,
                        "x_value": x_value,
                        "metric_name": mean_key,
                        "value": row[mean_key],
                    }
                )
    return rows


def build_comparison_rows(
    summary_rows: list[dict[str, Any]], previous_summary: dict[str, dict[str, str]]
) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for row in summary_rows:
        previous = previous_summary.get(row["case_id"])
        if previous is None:
            continue
        comparison_row: dict[str, Any] = {
            "case_id": row["case_id"],
            "case_kind": row["case_kind"],
            "io_threads": row["io_threads"],
            "connections": row["connections"],
            "payload_size_bytes": row["payload_size_bytes"],
            "outstanding_per_connection": row["outstanding_per_connection"],
            "reconnect_per_request": row["reconnect_per_request"],
            "method": row["method"],
        }
        for metric_name in ["qps_mean", "latency_avg_ms_mean", "latency_p99_ms_mean", "latency_p999_ms_mean"]:
            current = float(row[metric_name])
            previous_value = float(previous[metric_name])
            comparison_row[f"{metric_name}_current"] = current
            comparison_row[f"{metric_name}_previous"] = previous_value
            comparison_row[f"{metric_name}_delta_pct"] = (
                ((current - previous_value) / previous_value) * 100.0 if previous_value != 0 else ""
            )
        rows.append(comparison_row)
    return rows


def render_table(headers: list[str], rows: list[list[str]]) -> str:
    lines = [
        "| " + " | ".join(headers) + " |",
        "| " + " | ".join(["---"] * len(headers)) + " |",
    ]
    for row in rows:
        lines.append("| " + " | ".join(row) + " |")
    return "\n".join(lines)


def comparison_cell(
    current: dict[str, Any], previous: dict[str, str] | None, current_key: str, previous_key: str
) -> str:
    current_value = float(current[current_key])
    if previous is None or previous_key not in previous or not previous[previous_key]:
        return "n/a"
    baseline = float(previous[previous_key])
    if baseline == 0:
        return "n/a"
    delta = (current_value - baseline) / baseline * 100.0
    return format_percent(delta)


def build_markdown_report(
    args: argparse.Namespace,
    metadata: dict[str, Any],
    rows: list[dict[str, Any]],
    previous_summary: dict[str, dict[str, str]],
) -> str:
    by_kind: dict[str, list[dict[str, Any]]] = {
        "throughput": [],
        "thread_scale": [],
        "payload": [],
        "multiplex": [],
        "connection_mode": [],
        "handler": [],
    }
    for row in rows:
        by_kind[row["case_kind"]].append(row)

    lines = [
        "# Benchmark Report",
        "",
        "## Metadata",
        "",
        f"- Date: `{metadata['timestamp']}`",
        f"- Commit: `{metadata['commit']}`",
        f"- Label: `{metadata['label']}`",
        f"- Machine: `{metadata['machine']}`",
        f"- Method: `{metadata['method']}`",
        f"- Duration: `{metadata['duration_seconds']:.1f}s`",
        f"- Warmup: `{metadata['warmup_seconds']:.1f}s`",
        f"- Repeats: `{metadata['repeats']}`",
        "",
        "## Throughput And Tail Latency",
        "",
        render_table(
            [
                "Connections",
                "IO Threads",
                "Payload",
                "QPS",
                "P50",
                "P99",
                "P999",
                "Avg",
                "QPS vs Prev",
                "P99 vs Prev",
            ],
            [
                [
                    str(row["connections"]),
                    str(row["io_threads"]),
                    f"{row['payload_size_bytes']} B",
                    format_qps(float(row["qps_mean"])),
                    format_ms(float(row["latency_p50_ms_mean"])),
                    format_ms(float(row["latency_p99_ms_mean"])),
                    format_ms(float(row["latency_p999_ms_mean"])),
                    format_ms(float(row["latency_avg_ms_mean"])),
                    comparison_cell(row, previous_summary.get(row["case_id"]), "qps_mean", "qps_mean"),
                    comparison_cell(
                        row,
                        previous_summary.get(row["case_id"]),
                        "latency_p99_ms_mean",
                        "latency_p99_ms_mean",
                    ),
                ]
                for row in by_kind["throughput"]
            ],
        ),
        "",
        "## IO Thread Scaling",
        "",
        render_table(
            ["IO Threads", "Connections", "Payload", "QPS", "P50", "P99", "P999", "Avg"],
            [
                [
                    str(row["io_threads"]),
                    str(row["connections"]),
                    f"{row['payload_size_bytes']} B",
                    format_qps(float(row["qps_mean"])),
                    format_ms(float(row["latency_p50_ms_mean"])),
                    format_ms(float(row["latency_p99_ms_mean"])),
                    format_ms(float(row["latency_p999_ms_mean"])),
                    format_ms(float(row["latency_avg_ms_mean"])),
                ]
                for row in by_kind["thread_scale"]
            ],
        ),
        "",
        "## Payload Scaling",
        "",
        render_table(
            ["Payload", "IO Threads", "Connections", "QPS", "P50", "P99", "P999", "Avg"],
            [
                [
                    f"{row['payload_size_bytes']} B",
                    str(row["io_threads"]),
                    str(row["connections"]),
                    format_qps(float(row["qps_mean"])),
                    format_ms(float(row["latency_p50_ms_mean"])),
                    format_ms(float(row["latency_p99_ms_mean"])),
                    format_ms(float(row["latency_p999_ms_mean"])),
                    format_ms(float(row["latency_avg_ms_mean"])),
                ]
                for row in by_kind["payload"]
            ],
        ),
        "",
        "## Single Connection Multiplexing",
        "",
        render_table(
            ["Outstanding", "IO Threads", "Connections", "Payload", "QPS", "P50", "P99", "P999"],
            [
                [
                    str(row["outstanding_per_connection"]),
                    str(row["io_threads"]),
                    str(row["connections"]),
                    f"{row['payload_size_bytes']} B",
                    format_qps(float(row["qps_mean"])),
                    format_ms(float(row["latency_p50_ms_mean"])),
                    format_ms(float(row["latency_p99_ms_mean"])),
                    format_ms(float(row["latency_p999_ms_mean"])),
                ]
                for row in by_kind["multiplex"]
            ],
        ),
        "",
        "## Connection Reuse Vs Reconnect",
        "",
        render_table(
            ["Mode", "Connections", "Payload", "QPS", "P50", "P99", "P999"],
            [
                [
                    "reconnect/request" if row["reconnect_per_request"] else "reuse",
                    str(row["connections"]),
                    f"{row['payload_size_bytes']} B",
                    format_qps(float(row["qps_mean"])),
                    format_ms(float(row["latency_p50_ms_mean"])),
                    format_ms(float(row["latency_p99_ms_mean"])),
                    format_ms(float(row["latency_p999_ms_mean"])),
                ]
                for row in by_kind["connection_mode"]
            ],
        ),
        "",
        "## Handler Cost",
        "",
        render_table(
            ["Method", "Connections", "Outstanding", "Payload", "QPS", "P50", "P99", "P999"],
            [
                [
                    str(row["method"]),
                    str(row["connections"]),
                    str(row["outstanding_per_connection"]),
                    f"{row['payload_size_bytes']} B",
                    format_qps(float(row["qps_mean"])),
                    format_ms(float(row["latency_p50_ms_mean"])),
                    format_ms(float(row["latency_p99_ms_mean"])),
                    format_ms(float(row["latency_p999_ms_mean"])),
                ]
                for row in by_kind["handler"]
            ],
        ),
        "",
        "## Notes",
        "",
        "- `QPS/P50/P99/P999/Avg` are the arithmetic mean across the measured repeats.",
        "- `QPS vs Prev` and `P99 vs Prev` are computed only when `--compare-with` points to an older suite CSV.",
        "- Per-run raw outputs are stored in `raw_runs.json`; the aggregated machine-readable summary is stored in `summary.csv`.",
    ]
    return "\n".join(lines) + "\n"


def main() -> int:
    args = parse_args()
    client_path = Path(args.client)
    server_path = Path(args.server)
    if not args.skip_build_check:
        ensure_binary(client_path, "client")
        ensure_binary(server_path, "server")

    timestamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    label_suffix = f"-{sanitize_name(args.label)}" if args.label else ""
    output_dir = (
        Path(args.output_dir)
        if args.output_dir
        else DEFAULT_RESULTS_DIR / f"{timestamp}{label_suffix}"
    )
    output_dir.mkdir(parents=True, exist_ok=True)
    raw_dir = output_dir / "raw"
    metrics_prom_dir = raw_dir / "metrics_prom"
    server_logs_dir = raw_dir / "server_logs"
    raw_dir.mkdir(exist_ok=True)
    metrics_prom_dir.mkdir(exist_ok=True)
    server_logs_dir.mkdir(exist_ok=True)

    previous_summary: dict[str, dict[str, str]] = {}
    if args.compare_with:
        previous_summary = load_previous_summary(Path(args.compare_with))

    metadata = {
        "timestamp": dt.datetime.now().isoformat(timespec="seconds"),
        "commit": current_commit(),
        "label": args.label or "default",
        "machine": machine_summary(),
        "method": args.method,
        "duration_seconds": args.duration,
        "warmup_seconds": args.warmup,
        "repeats": args.repeats,
        "host": args.host,
        "port": args.port,
        "admin_port": args.admin_port,
        "metrics_sample_interval_seconds": args.metrics_sample_interval,
    }

    raw_runs: list[dict[str, Any]] = []
    summary_rows: list[dict[str, Any]] = []
    metric_snapshots: list[dict[str, Any]] = []

    cases: list[dict[str, Any]] = []
    for connections in args.connections:
        cases.append(
            {
                "case_kind": "throughput",
                "method": args.method,
                "io_threads": args.baseline_io_threads,
                "connections": connections,
                "payload_size_bytes": 16,
                "outstanding_per_connection": 1,
                "reconnect_per_request": False,
            }
        )
    for io_threads in args.io_threads:
        cases.append(
            {
                "case_kind": "thread_scale",
                "method": args.method,
                "io_threads": io_threads,
                "connections": args.baseline_connections,
                "payload_size_bytes": 16,
                "outstanding_per_connection": 1,
                "reconnect_per_request": False,
            }
        )
    for payload_size in args.payload_sizes:
        cases.append(
            {
                "case_kind": "payload",
                "method": args.method,
                "io_threads": args.baseline_io_threads,
                "connections": args.baseline_connections,
                "payload_size_bytes": payload_size,
                "outstanding_per_connection": 1,
                "reconnect_per_request": False,
            }
        )
    for outstanding in args.outstanding_per_connection:
        cases.append(
            {
                "case_kind": "multiplex",
                "method": args.method,
                "io_threads": args.baseline_io_threads,
                "connections": 1,
                "payload_size_bytes": 16,
                "outstanding_per_connection": outstanding,
                "reconnect_per_request": False,
            }
        )
    for reconnect_per_request in (False, True):
        cases.append(
            {
                "case_kind": "connection_mode",
                "method": args.method,
                "io_threads": args.baseline_io_threads,
                "connections": min(args.baseline_connections, 50),
                "payload_size_bytes": 16,
                "outstanding_per_connection": 1,
                "reconnect_per_request": reconnect_per_request,
            }
        )
    for method in ("EchoService.SlowEcho", "EchoService.CpuHeavy"):
        cases.append(
            {
                "case_kind": "handler",
                "method": method,
                "io_threads": args.baseline_io_threads,
                "connections": min(args.baseline_connections, 50),
                "payload_size_bytes": 256,
                "outstanding_per_connection": 1,
                "reconnect_per_request": False,
            }
        )

    for case in cases:
        cid = case_id(
            case["case_kind"],
            case["io_threads"],
            case["connections"],
            case["payload_size_bytes"],
            case["outstanding_per_connection"],
            case["reconnect_per_request"],
            case["method"],
        )
        server_log = server_logs_dir / f"{cid}.server.log"
        payload = build_payload(case["payload_size_bytes"])
        print(
            f"[bench] {cid}: io_threads={case['io_threads']} "
            f"connections={case['connections']} payload={case['payload_size_bytes']}B "
            f"outstanding={case['outstanding_per_connection']} "
            f"reconnect={case['reconnect_per_request']} method={case['method']}",
            flush=True,
        )

        server = launch_server(
            server_path=server_path,
            host=args.host,
            port=args.port,
            admin_port=args.admin_port,
            io_threads=case["io_threads"],
            log_path=server_log,
        )
        sampler = None
        if args.admin_port != 0 and args.metrics_sample_interval > 0:
            sampler = MetricsSampler(args.host, args.admin_port, args.metrics_sample_interval)
            sampler.switch_case(cid, case["case_kind"])
            sampler.start()

        runs: list[dict[str, Any]] = []
        case_succeeded = False
        try:
            if args.warmup > 0:
                if sampler is not None:
                    sampler.set_stage("warmup", 0)
                run_client(
                    client_path=client_path,
                    host=args.host,
                    port=args.port,
                    method=case["method"],
                    connections=case["connections"],
                    duration=args.warmup,
                    payload=payload,
                    outstanding_per_connection=case["outstanding_per_connection"],
                    reconnect_per_request=case["reconnect_per_request"],
                )
                if sampler is not None:
                    sampler.set_stage("idle", 0)
                if args.admin_port != 0:
                    metrics_text = http_get(args.host, args.admin_port, "/metrics")
                    (metrics_prom_dir / f"{cid}.warmup.metrics.prom").write_text(
                        metrics_text, encoding="utf-8"
                    )
                    scalars, labeled = parse_metrics_text(metrics_text)
                    metric_snapshots.append(
                        {
                            "sample_timestamp": dt.datetime.now().isoformat(
                                timespec="milliseconds"
                            ),
                            "case_id": cid,
                            "case_kind": case["case_kind"],
                            "stage": "warmup",
                            "repeat": 0,
                            "scalars": scalars,
                            "labeled": labeled,
                        }
                    )

            for repeat in range(1, args.repeats + 1):
                if sampler is not None:
                    sampler.set_stage("repeat", repeat)
                run = run_client(
                    client_path=client_path,
                    host=args.host,
                    port=args.port,
                    method=case["method"],
                    connections=case["connections"],
                    duration=args.duration,
                    payload=payload,
                    outstanding_per_connection=case["outstanding_per_connection"],
                    reconnect_per_request=case["reconnect_per_request"],
                )
                if sampler is not None:
                    sampler.set_stage("idle", repeat)
                run["repeat"] = repeat
                run["case_id"] = cid
                run["case_kind"] = case["case_kind"]
                run["method"] = case["method"]
                run["io_threads"] = case["io_threads"]
                run["connections"] = case["connections"]
                run["payload_size_bytes"] = case["payload_size_bytes"]
                run["outstanding_per_connection"] = case["outstanding_per_connection"]
                run["reconnect_per_request"] = case["reconnect_per_request"]
                if args.admin_port != 0:
                    metrics_text = http_get(args.host, args.admin_port, "/metrics")
                    (metrics_prom_dir / f"{cid}.repeat{repeat}.metrics.prom").write_text(
                        metrics_text, encoding="utf-8"
                    )
                    scalars, labeled = parse_metrics_text(metrics_text)
                    run["metrics_scalars"] = scalars
                    metric_snapshots.append(
                        {
                            "sample_timestamp": dt.datetime.now().isoformat(
                                timespec="milliseconds"
                            ),
                            "case_id": cid,
                            "case_kind": case["case_kind"],
                            "stage": "repeat",
                            "repeat": repeat,
                            "scalars": scalars,
                            "labeled": labeled,
                        }
                    )
                raw_runs.append(run)
                runs.append(run)
            case_succeeded = True
        finally:
            if sampler is not None:
                sampler.stop()
                metric_snapshots.extend(sampler.samples)
                if sampler.errors:
                    (raw_dir / f"{cid}.metrics_sampler_errors.log").write_text(
                        "\n".join(sampler.errors) + "\n", encoding="utf-8"
                    )
            stop_server(server)
            if not args.keep_server_logs and case_succeeded and server_log.exists():
                server_log.unlink()

        summary = summarize_runs(runs)
        row = {
            "case_id": cid,
            "case_kind": case["case_kind"],
            "method": case["method"],
            "io_threads": case["io_threads"],
            "connections": case["connections"],
            "payload_size_bytes": case["payload_size_bytes"],
            "outstanding_per_connection": case["outstanding_per_connection"],
            "reconnect_per_request": case["reconnect_per_request"],
            **summary,
        }
        case_metric_snapshots = [
            snapshot
            for snapshot in metric_snapshots
            if snapshot["case_id"] == cid and snapshot["stage"] == "repeat"
        ]
        row.update(summarize_metric_snapshots(case_metric_snapshots))
        summary_rows.append(row)

    summary_rows.sort(
        key=lambda row: (
            row["case_kind"],
            row["method"],
            row["io_threads"],
            row["connections"],
            row["payload_size_bytes"],
            row["outstanding_per_connection"],
            row["reconnect_per_request"],
        )
    )

    write_json(output_dir / "metadata.json", metadata)
    write_json(raw_dir / "raw_runs.json", raw_runs)
    write_json(raw_dir / "metrics_snapshots.json", metric_snapshots)
    write_csv(output_dir / "summary.csv", summary_rows)
    write_csv(output_dir / "measurements.csv", build_measurement_rows(raw_runs))
    write_csv(output_dir / "method_metrics.csv", build_method_metric_rows(metric_snapshots))
    write_csv(output_dir / "metrics_timeseries.csv", build_metrics_timeseries_rows(metric_snapshots))
    write_csv(output_dir / "plot_series.csv", build_plot_rows(summary_rows))
    if previous_summary:
        write_csv(output_dir / "comparison.csv", build_comparison_rows(summary_rows, previous_summary))
    report = build_markdown_report(args, metadata, summary_rows, previous_summary)
    (output_dir / "report.md").write_text(report, encoding="utf-8")

    print(f"[bench] report written to {output_dir / 'report.md'}")
    print(f"[bench] summary csv written to {output_dir / 'summary.csv'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
