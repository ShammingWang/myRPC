# myRPC

`myRPC` 是一个基于 `C++` 和 `Linux socket/epoll` 的轻量 RPC 框架原型项目。

这个项目的目标不是只做“能收发请求的 demo”，而是逐步补齐一个基础 RPC 运行时最重要的几块能力：

- 多 IO 线程网络模型
- 业务线程池与请求分发
- 请求超时、连接治理与背压
- `/metrics`、slow log、request trace 等可观测性
- 可重复执行的 benchmark suite 与性能基线

当前代码已经具备一条完整的最小闭环，适合继续做线程模型、协议路径、序列化和尾延迟优化实验。

## Snapshot

- 网络模型：主线程 `accept` + 多 IO 线程 `epoll` reactor
- 请求执行：`WorkerPool` 异步执行 handler，响应回投到原 IO 线程
- 协议模型：自定义 MRPC 二进制协议，使用 `request_id` 关联请求与响应
- 可观测性：`/metrics`、slow log、request trace
- 压测体系：正式 `C++ benchmark client` + 固定矩阵的 `bench/run_suite.py`
- 当前 benchmark 基线：`8` 个 IO 线程下约 `45k-50k QPS`，`400` 连接时尾延迟开始明显放大

## Project Layout

```text
rpc-project/
├── CMakeLists.txt
├── README.md
├── docs/
│   ├── architecture.md
│   ├── protocol.md
│   └── benchmark.md
├── src/
│   ├── server/
│   ├── codec/
│   ├── dispatcher/
│   ├── connection/
│   └── worker/
├── include/
├── examples/
│   └── simple_client.cpp
├── bench/
│   ├── cpp_bench_client.cpp
│   ├── run_bench.sh
│   ├── cases/
│   └── results/
├── tests/
│   ├── unit/
│   └── integration/
```

## 当前能力

- 监听指定端口
- 主线程 `accept` + 多个 IO 线程基于 `epoll` 处理连接读写
- 业务线程池异步执行 handler，IO 线程只负责网络收发和编解码
- 并发处理多个客户端连接
- Service / Method 服务模型，支持 `EchoService.Echo` 这类方法命名
- 正式 `RpcClient`，支持连接管理、请求发送、响应匹配与超时控制
- 二进制长度字段帧协议，支持序列化类型与请求超时字段
- `request_id` 关联请求和响应
- `Connection + Codec + Dispatcher` 三层结构
- 单连接内连续处理多条 RPC 请求
- 连接级背压控制：最大 in-flight 请求数、写缓冲上限
- 空闲连接回收与请求超时处理
- 可插拔序列化元数据，内置 `raw` 和 `json`
- 更细的错误码体系，区分协议、序列化、超时和网络错误
- 内建 observability：周期性 metrics、slow log、request trace

## 架构概览

核心请求路径如下：

1. 主线程接收 TCP 连接，并按 round-robin 分发给某个 IO 线程。
2. IO 线程把连接注册到自己的 `epoll` 实例，负责后续读写事件。
3. `Connection` 从读缓冲中取字节流，交给 `RpcCodec` 解析为 `RpcRequest`。
4. `Server` 将请求投递到 `WorkerPool`，业务 handler 在工作线程中执行。
5. `RpcDispatcher` 按 `Service.Method` 查找方法并生成 `RpcResponse`。
6. 响应经 `eventfd` 回投到目标 IO 线程，由 `Connection` 编码并写回客户端。
7. `Observability` 在 decode、分发、回投、发送等节点汇总指标与日志。

更完整说明见 [architecture.md](/home/shamming/projects/myRPC/docs/architecture.md)。

## 构建

```bash
cmake -S . -B build
cmake --build build
```

## 运行

```bash
./build/simple_tcp_server
```

默认监听 `0.0.0.0:8080`。

可选环境变量：

- `MRPC_IO_THREADS`：指定 IO 线程数
- `MRPC_ADMIN_PORT`：管理 HTTP 端口，默认 `9090`，设置为 `0` 可关闭
- `MRPC_SLOW_REQUEST_MS`：慢请求阈值，默认 `50`
- `MRPC_ENABLE_REQUEST_TRACE`：是否开启 request trace，默认 `true`
- `MRPC_TRACE_ALL_REQUESTS`：是否打印所有请求 trace，默认 `false`

服务启动后：

- RPC 服务默认监听 `0.0.0.0:8080`
- 管理端口默认监听 `0.0.0.0:9090`
- 可通过 `http://127.0.0.1:9090/metrics` 查看当前指标

## 协议格式

请求和响应都使用固定 34 字节包头：

```text
4 bytes   magic      固定为 "MRPC"
1 byte    version    当前为 1
1 byte    type       1=request, 2=response
1 byte    serialization 0=raw, 1=json
1 byte    reserved
2 bytes   status     请求时固定为 0，响应时表示状态码
4 bytes   method_len 请求方法名长度，响应固定为 0
4 bytes   body_len   负载长度
4 bytes   timeout_ms 请求超时时间，响应固定为 0
4 bytes   reserved
8 bytes   request_id 请求 ID
```

请求帧正文为：

```text
method bytes + payload bytes
```

响应帧正文为：

```text
payload bytes
```

## 最小测试

可以直接跑项目里的正式客户端示例：

```bash
./build/simple_client
```

## Benchmark

项目内测试与性能回归统一走 `bench/` 目录下的 C++ client 和 suite 脚本。

项目内已经整理出一套 benchmark 目录，正式压测入口是 C++ 版 client：

```text
bench/
├── cpp_bench_client.cpp
├── run_bench.sh
├── cases/
└── results/
```

### 快速单次压测

```bash
./build/simple_tcp_server
```

另开一个终端执行：

```bash
./build/mrpc_bench_client \
  --connections 50 \
  --duration 8 \
  --method EchoService.Echo \
  --payload "hello rpc" \
  --expect-payload "hello rpc"
```

如果需要机器可读结果：

```bash
./build/mrpc_bench_client \
  --connections 50 \
  --duration 8 \
  --method EchoService.Echo \
  --payload "hello rpc" \
  --expect-payload "hello rpc" \
  --report-format json
```

### 正式 Benchmark Suite

推荐的正式入口是：

```bash
python3 bench/run_suite.py --label baseline
```

它会自动：

- 为每个 case 启动独立服务端实例
- 默认开启 admin 端口并抓取 `/metrics`
- 固定执行 `throughput / io thread scaling / payload scaling` 三组测试
- 对每个 case 先 warmup，再做 measured repeats
- 生成 `metadata.json / raw_runs.json / metrics_snapshots.json / summary.csv / measurements.csv / plot_series.csv / report.md`

默认矩阵：

- Throughput：`connections=50,100,200,400`，`payload=16 B`
- IO thread scaling：`io_threads=1,2,4,8,16`，`connections=200`
- Payload scaling：`payload=16,256,4096,16384 B`，`connections=200`

重要字段含义：

- `Connections`：客户端同时建立并复用的并发 TCP 连接数
- `IO Threads`：服务端 IO 线程数量，对应 `MRPC_IO_THREADS`
- `QPS`：每秒成功请求数
- `P50 / P99 / P999`：中位延迟、尾延迟、极尾延迟

额外导出的结果文件：

- `measurements.csv`：每次 measured run 的扁平化结果，适合直接画图
- `plot_series.csv`：长表格式绘图输入
- `method_metrics.csv`：按 `method` 维度导出的指标快照
- `metrics_snapshots.json`：suite 自动抓取到的 `/metrics`
- `metrics_timeseries.csv`：压测进行中周期采样得到的时序指标

目录组织约定：

- 顶层结果目录保留 `report / summary / plotting` 等核心产物
- 原始快照放到 `raw/` 下
- Prometheus 原始指标文本放到 `raw/metrics_prom/`

推荐流程：

1. 跑一版基线：`python3 bench/run_suite.py --label baseline`
2. 做代码优化
3. 跑新版本并和旧结果对比：

```bash
python3 bench/run_suite.py \
  --label after-opt \
  --compare-with bench/results/<baseline-dir>/summary.csv
```

更多细节见 [benchmark.md](/home/shamming/projects/myRPC/docs/benchmark.md)。

查看参数：

```bash
./build/mrpc_bench_client --help
python3 bench/run_suite.py --help
```

## Observability

服务端现在会输出三类观测日志：

- `/metrics`：独立 HTTP 管理端口导出当前连接、请求、错误、超时等指标
- `slow`：超过阈值的慢请求，包含 `queue_ms / handler_ms / io_return_ms / end_to_end_ms`
- `trace`：失败请求或开启全量 trace 时打印，带 `request_id / method / client / io_thread / connection_id`

## 当前 Baseline

当前正式 benchmark 基线保存在：

- [bench/results/20260422-002612-baseline/report.md](/home/shamming/projects/myRPC/bench/results/20260422-002612-baseline/report.md)
- [bench/results/20260422-002612-baseline/summary.csv](/home/shamming/projects/myRPC/bench/results/20260422-002612-baseline/summary.csv)

测试环境：

- 日期：`2026-04-22`
- 测试机器：`WSL2 x86_64`，`16 vCPU`
- 服务方法：`EchoService.Echo`
- payload 矩阵：`16 B / 256 B / 4096 B / 16384 B`
- benchmark duration：`6s`
- repeats：`3`

本轮结果概览：

- `8` 个 IO 线程、`50-200` 连接下，吞吐大致稳定在 `48k-50k QPS`
- `400` 连接时，吞吐下降到约 `45.5k QPS`，`P99` 升到约 `14.7 ms`
- `IO Threads` 从 `1 -> 8` 时收益明显，`8 -> 16` 时吞吐提升已经很有限
- payload 从 `16 B` 增长到 `16 KB` 时，QPS 从约 `46.6k` 下降到约 `39.3k`

这组结果说明当前多 IO 线程架构已经有效，但高连接数场景下尾延迟仍有优化空间。

## 适合继续优化的方向

- 高连接数下的排队与唤醒路径，重点看 `400` 连接场景的 `P99 / P999`
- 编解码与缓冲区管理，重点看 `16 KB` payload 场景
- worker 回投路径和锁竞争，重点看 `8 -> 16` IO 线程时收益变小的问题
- 后续可补 `perf`、CPU 利用率和 `/metrics` 快照，把 benchmark 和 profiling 串起来

## 下一步建议

1. 给 `RpcClient` 增加连接池、自动重连和异步调用接口。
2. 把 benchmark suite 和 `/metrics` 快照结合起来，保留每轮性能测试的运行时指标。
3. 补充更重 handler、异常场景和过载场景下的稳定性测试。
4. 把 `json` serializer 从“轻校验”升级成真正的结构化序列化。
