# Benchmark Notes

这个文档用于记录 `myRPC` benchmark 的测试口径、执行命令和阶段性结果，方便后续做横向对比。当前推荐入口是 `bench/run_suite.py`，它会用同一套固定矩阵生成可重复的报告。

## 目录结构

```text
bench/
├── run_suite.py
├── cpp_bench_client.cpp
├── run_bench.sh
├── cases/
└── results/
```

## 构建

```bash
cmake -S . -B build
cmake --build build
```

## 快速单次压测

先启动服务端：

```bash
./build/simple_tcp_server
```

使用 C++ benchmark client：

```bash
./build/mrpc_bench_client --connections 50 --duration 8 --method EchoService.Echo --payload "hello rpc"
```

如果需要机器可读输出，可以加 `--report-format json`。

## 正式 Benchmark Suite

推荐使用统一入口：

```bash
python3 bench/run_suite.py --label observability-baseline
```

这条命令会自动：

- 为每个 case 启动独立服务端实例
- 默认开启 admin 端口并自动抓取 `/metrics`
- 固定使用同一套 `throughput / io thread scaling / payload scaling / multiplexing / connection mode / handler cost` 矩阵
- 每个 case 先跑 warmup，再跑多次 measured repeats
- 生成 `metadata.json / raw_runs.json / metrics_snapshots.json / summary.csv / measurements.csv / plot_series.csv / report.md`

默认矩阵如下：

- Throughput sweep：`connections=50,100,200,400`，`payload=16 B`
- IO thread scaling：`io_threads=1,2,4,8,16`，`connections=200`，`payload=16 B`
- Payload sweep：`payload=16,256,4096,16384,65536 B`，`connections=200`
- Single-connection multiplexing：`connections=1`，`outstanding_per_connection=1,4,16,64`
- Connection mode：长连接复用 vs `--reconnect-per-request`
- Handler cost：`EchoService.SlowEcho` 与 `EchoService.CpuHeavy`

常用参数：

- `--duration 6 --warmup 2 --repeats 3`：控制每个 case 的测试时长和重复次数
- `--baseline-io-threads 8`：设置默认基线 IO 线程数
- `--outstanding-per-connection 1,4,16,64`：设置单连接 pipeline / multiplexing 压测深度
- `--admin-port 9090`：开启管理端口并抓取 `/metrics`，设置为 `0` 可关闭
- `--metrics-sample-interval 0.5`：压测进行中每隔多少秒抓一次 `/metrics`，设置为 `0` 可关闭周期采样
- `--compare-with bench/results/<old>/summary.csv`：和上一次报告做前后对比
- `--output-dir bench/results/<name>`：自定义结果输出目录

推荐对比流程：

1. 在当前基线版本执行一次 suite，保留生成的 `summary.csv`
2. 做优化改动
3. 再执行一次 suite，并把旧的 `summary.csv` 传给 `--compare-with`
4. 直接查看新生成的 `report.md` 中的 `QPS vs Prev / P99 vs Prev`

## 字段定义

正式 benchmark 报告里最常见的字段含义如下：

- `Connections`
  客户端同时建立并复用的并发 TCP 连接数。当前 `mrpc_bench_client` 默认每个 worker 对应一条长连接，单连接上 `in-flight=1`。
- `IO Threads`
  服务端 IO 线程数量，对应环境变量 `MRPC_IO_THREADS`。这不是客户端线程数，也不是 worker pool 线程数。
- `QPS`
  每秒成功请求数，只统计返回成功的请求吞吐。
- `Latency avg / p50 / p90 / p99 / p999`
  请求端到端延迟分位数，`p99 / p999` 更适合观察高压下的尾延迟变化。
- `Tx / Rx throughput`
  benchmark client 观测到的发送与接收吞吐，单位为 `KiB/s`。
- `Failures`
  本轮压测中发生的失败请求数；正式 baseline 应优先保证它保持为 `0`。

## 为什么固定这三类维度

当前 suite 选择 `throughput / io thread scaling / payload scaling / multiplexing / connection mode / handler cost`，是为了分别回答几类问题：

- `throughput`
  固定 `IO Threads` 和 payload，只改变 `Connections`，用来观察系统在负载上升时的吞吐上限和尾延迟变化。
- `io thread scaling`
  固定 `Connections` 和 payload，只改变服务端 IO 线程数，用来验证多 IO 线程架构的扩展性，以及最佳线程数大概落在哪个区间。
- `payload scaling`
  固定 `Connections` 和 `IO Threads`，只改变 payload 大小，用来观察协议路径、拷贝和缓冲区成本随消息体增长的变化。
- `single-connection multiplexing`
  固定为单条长连接，只改变单连接 outstanding request 数，用来验证 `request_id` 多路复用能力和单连接队头/排队影响。
- `connection mode`
  对比长连接复用与每请求重连，用来量化连接建立、accept、fd 生命周期管理对吞吐和尾延迟的影响。
- `handler cost`
  用慢 handler 和 CPU-heavy handler 观察瓶颈从网络路径迁移到 worker pool 或业务执行路径时的指标变化。

这些 case 共同组成一套“最小但有效”的性能基线，能在不引入太多变量的前提下，帮助定位下一步优化方向。

## 建议记录项

- 测试日期
- 分支或提交号
- 测试机器配置
- 服务端日志级别
- 是否连接复用
- connections / requests / duration
- payload 大小
- QPS / avg / p50 / p90 / p99 / p999 / throughput / failures

## 结果模板

`bench/run_suite.py` 会自动生成：

- `metadata.json`：测试环境、commit、参数
- `raw/raw_runs.json`：每次 measured run 的原始结果
- `summary.csv`：按 case 聚合后的机器可读汇总
- `measurements.csv`：每次 measured run 的平铺结果，适合直接画图
- `plot_series.csv`：长表格式的绘图输入
- `method_metrics.csv`：按 `method` 维度导出的指标快照
- `raw/metrics_snapshots.json`：每个 case warmup / repeat 后抓取到的 `/metrics`
- `metrics_timeseries.csv`：压测进行中周期采样得到的时序指标，适合画运行中曲线
- `raw/metrics_prom/`：每个 case 每个阶段保存的 Prometheus 原始文本快照
- `report.md`：适合提交到仓库的 Markdown 报告

建议把顶层目录当作“主要结果入口”，需要排查细节时再进入 `raw/` 查看原始快照。

## 说明

- 统一 suite 的目标是保证“每次优化都跑同一套基准”，先稳定复现，再讨论进一步的 profiling。
- 当前 suite 聚焦回环网络下框架内部路径的对比，更适合评估线程模型、协议路径和序列化/拷贝成本。
- `bench/profile_perf.sh` 可用于采集 `perf record`、`perf report`、客户端 JSON 和 `/metrics` 快照；如果本机安装了 FlameGraph 脚本，也会生成 `flamegraph.svg`。
- 当前项目的测试和性能回归统一走 `bench/` 目录，不再维护 `scripts/` 下的辅助压测入口。

## Perf Profile

默认 profile 一次 `EchoService.Echo`、`200` 长连接、`16 B` payload、`20s`：

```bash
bench/profile_perf.sh
```

运行前需要本机 PATH 中存在 `perf`；如果要自动生成 SVG 火焰图，还需要 `stackcollapse-perf.pl` 和 `flamegraph.pl`，或者设置 `FLAMEGRAPH_DIR`。
为了让 perf 符号更可读，建议使用 `-DCMAKE_BUILD_TYPE=RelWithDebInfo` 构建。

常用参数通过环境变量覆盖：

```bash
IO_THREADS=8 \
CONNECTIONS=1 \
OUTSTANDING_PER_CONNECTION=64 \
PAYLOAD_SIZE=16 \
DURATION_SECONDS=20 \
LABEL=multiplex-out64 \
bench/profile_perf.sh
```

输出目录默认在 `bench/results/<timestamp>-<label>/`，其中：

- `client.json`：本次 profile 的压测结果
- `metrics.prom`：profile 结束后的 `/metrics`
- `perf.report.txt`：热点文本报告
- `perf.data`：原始 perf 数据
- `flamegraph.svg`：可用时自动生成

## 2026-04-11 Single IO vs Multi IO

这一组结果用于对比服务端从“单线程 IO”改造成“主线程 `accept` + 多 IO 线程”后的收益。

### 测试口径

- 日期：`2026-04-11`
- 测试机器：当前开发环境，`16 vCPU`
- 服务方法：`EchoService.Echo`
- payload：`hello rpc`，大小 `9 B`
- benchmark client：项目内 `mrpc_bench_client`
- 压测模式：固定时长 `duration=6s`
- 每组重复两次，表格内为平均值
- 连接复用开启，不使用 `--reconnect-per-request`

### 对照组

- `baseline single-io`：改造前基线版本 `bffdd75`
- `multi-io(1)`：新架构，但限制为 `1` 个 IO 线程
- `multi-io(16)`：新架构，使用 `16` 个 IO 线程

### 对比结果

| Connections | Baseline QPS | Multi-io(1) QPS | Multi-io(16) QPS | Baseline Avg | Multi-io(1) Avg | Multi-io(16) Avg | Baseline P99 | Multi-io(1) P99 | Multi-io(16) P99 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 50 | 13793.31 | 13326.24 | 47594.32 | 3.619 ms | 3.746 ms | 1.046 ms | 6.209 ms | 5.540 ms | 3.338 ms |
| 100 | 13730.40 | 13531.55 | 48179.05 | 7.268 ms | 7.376 ms | 2.069 ms | 10.128 ms | 10.088 ms | 5.760 ms |
| 200 | 13347.12 | 13506.27 | 45729.07 | 14.945 ms | 14.765 ms | 4.358 ms | 19.569 ms | 18.032 ms | 10.558 ms |
| 400 | 13209.30 | 13446.99 | 43754.54 | 30.117 ms | 29.580 ms | 9.095 ms | 42.678 ms | 38.951 ms | 18.410 ms |

### 结论

- 新架构在 `io_thread_count=1` 时与基线版本基本持平，说明重构本身没有带来明显额外开销。
- 当 `io_thread_count=16` 时，吞吐显著提升，`QPS` 大约提升 `2.3x ~ 2.5x`。
- 平均延迟下降约 `70%`，`P99` 下降约 `43% ~ 57%`。
- 对当前这种轻业务逻辑、连接数较高的场景，多 IO 线程模型已经明显优于单 IO 线程模型。

### 相对 Baseline 的收益

| Connections | QPS Gain | Avg Latency Drop | P99 Drop |
| --- | ---: | ---: | ---: |
| 50 | 245.1% | 71.1% | 46.2% |
| 100 | 250.9% | 71.5% | 43.1% |
| 200 | 242.6% | 70.8% | 46.0% |
| 400 | 231.2% | 69.8% | 56.9% |

### 说明

- 这组测试在本机回环网络上完成，更能体现框架内部线程模型和收发路径的差异。
- 当前 handler 较轻，主要瓶颈集中在网络事件处理，因此更容易观察到多 IO 线程带来的扩展性收益。
- 如果后续引入更重的业务逻辑或更复杂的序列化，瓶颈可能会进一步转移到 `WorkerPool` 或编解码路径。
