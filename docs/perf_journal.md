# Performance Journal

这个文档记录 `myRPC` 的性能优化闭环。每一条记录都应该能回答：

- 问题是什么
- 用什么证据定位
- 改了什么
- 结果是否变好

建议每次优化都先跑 baseline，再跑 optimized，并把 `summary.csv`、`report.md`、`perf.report.txt` 或 `flamegraph.svg` 路径写在记录里。

## Workflow

### 1. Benchmark Baseline

```bash
cmake -S . -B build
cmake --build build
python3 bench/run_suite.py --label baseline
```

做热点分析时建议使用带符号的构建：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
```

### 2. Profile Hot Path

```bash
bench/profile_perf.sh
```

运行前需要本机 PATH 中存在 `perf`；如果要自动生成 SVG 火焰图，还需要 `stackcollapse-perf.pl` 和 `flamegraph.pl`，或者设置 `FLAMEGRAPH_DIR`。

常用覆盖参数：

```bash
IO_THREADS=8 \
CONNECTIONS=200 \
OUTSTANDING_PER_CONNECTION=1 \
PAYLOAD_SIZE=4096 \
DURATION_SECONDS=20 \
LABEL=payload-4k \
bench/profile_perf.sh
```

输出目录默认位于 `bench/results/<timestamp>-<label>/`，核心文件包括：

- `client.json`：本次 profile 对应的 QPS、延迟和吞吐
- `metrics.prom`：profile 后抓取的 `/metrics`
- `perf.data`：原始 perf 数据
- `perf.report.txt`：文本热点报告
- `perf.script.txt`：火焰图输入
- `flamegraph.svg`：如果本机存在 FlameGraph 脚本则自动生成

### 3. Compare Optimized Build

```bash
python3 bench/run_suite.py \
  --label optimized \
  --compare-with bench/results/<baseline>/summary.csv
```

## Entry Template

### YYYY-MM-DD Short Title

- Problem:
- Hypothesis:
- Baseline:
  - Benchmark report:
  - Perf output:
  - Key metrics:
- Findings:
  - Benchmark evidence:
  - `/metrics` evidence:
  - `perf` / flamegraph evidence:
- Change:
  - Code:
  - Config:
- Result:
  - QPS:
  - Avg latency:
  - P99 latency:
  - CPU / perf:
- Decision:
  - Keep / revert / continue investigating

## 2026-04-29 Performance Loop Setup

- Problem: benchmark suite had throughput, IO-thread, and payload sweeps, but did not yet cover perf profiling, single-connection multiplexing, reconnect cost, or handler-cost scenarios.
- Hypothesis: adding these dimensions will make regressions easier to explain and will expose whether bottlenecks sit in network IO, connection setup, worker execution, or payload copy paths.
- Baseline:
  - Existing suite: `throughput / thread_scale / payload`
  - Existing profile tooling: none
- Change:
  - Added `bench/profile_perf.sh` for server-scoped `perf record`, `perf report`, optional FlameGraph SVG, client JSON, and `/metrics` capture.
  - Extended `bench/run_suite.py` with:
    - `multiplex`: single connection with multiple outstanding requests
    - `connection_mode`: long connection reuse vs reconnect-per-request
    - `handler`: slow handler and CPU-heavy handler scenarios
    - 64 KiB payload sweep
  - Extended `mrpc_bench_client` with `--outstanding-per-connection`.
  - Added `EchoService.CpuHeavy` for CPU-bound handler benchmarking.
- Result:
  - Pending first full run.
- Decision:
  - Keep this as the standard performance workflow before starting protocol, buffer, or syscall-path optimizations.

## 2026-04-29 First Perf Profile

- Problem: establish the first server-side perf evidence for the current hot path before changing protocol, buffer, or queueing code.
- Hypothesis: under lightweight `EchoService.Echo`, cost should concentrate in IO thread read/decode, worker dispatch/response enqueue, IO wakeup, and response writeback rather than handler execution.
- Baseline:
  - Multi-connection profile: `bench/results/profile-echo-conn200`
  - Single-connection multiplex profile: `bench/results/profile-echo-out64`
- Findings:
  - `conn200`: `37024.53 QPS`, avg `5.389 ms`, p99 `11.110 ms`, p999 `14.183 ms`, failures `0`.
  - `out64`: `28340.98 QPS`, avg `2.257 ms`, p99 `2.637 ms`, p999 `3.091 ms`, failures `0`.
  - `conn200` `/metrics`: queue avg `0.020 ms`, handler avg `0.002 ms`, return queue avg `1.271 ms`, send avg `2.714 ms`, e2e avg `4.015 ms`.
  - `out64` `/metrics`: queue avg `0.017 ms`, handler avg `0.002 ms`, return queue avg `0.570 ms`, send avg `0.093 ms`, e2e avg `0.686 ms`.
  - `conn200` perf: IO threads dominate; visible cost in `Connection::OnReadable`, `Connection::ProcessRequests`, `Server::DispatchRequest`, `WorkerPool::Submit`, method metrics update, allocation for task capture, and response path.
  - `out64` perf: worker threads dominate; visible cost in `Server::EnqueueResponse`, `std::queue<PendingResponse>::push`, `Server::WakeIoThread`, and IO-thread stats updates.
- Change:
  - No runtime code change in this entry; this is the measurement baseline.
- Result:
  - Evidence points to response回投/wakeup/queueing and observability bookkeeping as the first optimization candidates, not handler execution.
- Decision:
  - Next optimization should target response batching or cheaper IO-thread wakeups, then compare against both profile directories and the suite `summary.csv`.
