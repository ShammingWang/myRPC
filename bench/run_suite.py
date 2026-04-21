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
import time
from pathlib import Path
from typing import Any


ROOT_DIR = Path(__file__).resolve().parent.parent
BUILD_DIR = ROOT_DIR / "build"
DEFAULT_SERVER = BUILD_DIR / "simple_tcp_server"
DEFAULT_CLIENT = BUILD_DIR / "mrpc_bench_client"
DEFAULT_RESULTS_DIR = ROOT_DIR / "bench" / "results"


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
    parser.add_argument("--admin-port", type=int, default=0, help="server admin port")
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
        default=parse_int_list("16,256,4096,16384"),
        help="payload sizes in bytes for the payload sweep",
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


def case_id(kind: str, io_threads: int, connections: int, payload_size: int) -> str:
    return f"{kind}__io{io_threads}__conn{connections}__payload{payload_size}"


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
        "--report-format",
        "json",
    ]
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
    fieldnames = list(rows[0].keys())
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


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
    by_kind: dict[str, list[dict[str, Any]]] = {"throughput": [], "thread_scale": [], "payload": []}
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
    }

    raw_runs: list[dict[str, Any]] = []
    summary_rows: list[dict[str, Any]] = []

    cases: list[dict[str, Any]] = []
    for connections in args.connections:
        cases.append(
            {
                "case_kind": "throughput",
                "io_threads": args.baseline_io_threads,
                "connections": connections,
                "payload_size_bytes": 16,
            }
        )
    for io_threads in args.io_threads:
        cases.append(
            {
                "case_kind": "thread_scale",
                "io_threads": io_threads,
                "connections": args.baseline_connections,
                "payload_size_bytes": 16,
            }
        )
    for payload_size in args.payload_sizes:
        cases.append(
            {
                "case_kind": "payload",
                "io_threads": args.baseline_io_threads,
                "connections": args.baseline_connections,
                "payload_size_bytes": payload_size,
            }
        )

    for case in cases:
        cid = case_id(
            case["case_kind"],
            case["io_threads"],
            case["connections"],
            case["payload_size_bytes"],
        )
        server_log = output_dir / f"{cid}.server.log"
        payload = build_payload(case["payload_size_bytes"])
        print(
            f"[bench] {cid}: io_threads={case['io_threads']} "
            f"connections={case['connections']} payload={case['payload_size_bytes']}B",
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

        runs: list[dict[str, Any]] = []
        case_succeeded = False
        try:
            if args.warmup > 0:
                run_client(
                    client_path=client_path,
                    host=args.host,
                    port=args.port,
                    method=args.method,
                    connections=case["connections"],
                    duration=args.warmup,
                    payload=payload,
                )

            for repeat in range(1, args.repeats + 1):
                run = run_client(
                    client_path=client_path,
                    host=args.host,
                    port=args.port,
                    method=args.method,
                    connections=case["connections"],
                    duration=args.duration,
                    payload=payload,
                )
                run["repeat"] = repeat
                run["case_id"] = cid
                run["case_kind"] = case["case_kind"]
                run["io_threads"] = case["io_threads"]
                run["connections"] = case["connections"]
                raw_runs.append(run)
                runs.append(run)
            case_succeeded = True
        finally:
            stop_server(server)
            if not args.keep_server_logs and case_succeeded and server_log.exists():
                server_log.unlink()

        summary = summarize_runs(runs)
        row = {
            "case_id": cid,
            "case_kind": case["case_kind"],
            "io_threads": case["io_threads"],
            "connections": case["connections"],
            "payload_size_bytes": case["payload_size_bytes"],
            **summary,
        }
        summary_rows.append(row)

    summary_rows.sort(key=lambda row: (row["case_kind"], row["io_threads"], row["connections"], row["payload_size_bytes"]))

    write_json(output_dir / "metadata.json", metadata)
    write_json(output_dir / "raw_runs.json", raw_runs)
    write_csv(output_dir / "summary.csv", summary_rows)
    report = build_markdown_report(args, metadata, summary_rows, previous_summary)
    (output_dir / "report.md").write_text(report, encoding="utf-8")

    print(f"[bench] report written to {output_dir / 'report.md'}")
    print(f"[bench] summary csv written to {output_dir / 'summary.csv'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
