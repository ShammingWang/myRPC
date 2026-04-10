# Benchmark Notes

这个文档用于记录 `myRPC` benchmark 的测试口径、执行命令和阶段性结果，方便后续做横向对比。

## 目录结构

```text
bench/
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

## 运行方式

先启动服务端：

```bash
./build/simple_tcp_server
```

Python 版本：

```bash
python3 scripts/bench_python.py --connections 50 --duration 8 --method EchoService.Echo --payload "hello rpc"
```

C++ 版本：

```bash
./build/mrpc_bench_client --connections 50 --duration 8 --method EchoService.Echo --payload "hello rpc"
```

## 建议记录项

- 测试日期
- 分支或提交号
- 测试机器配置
- 服务端日志级别
- 是否连接复用
- connections / requests / duration
- payload 大小
- QPS / avg / p50 / p99 / throughput / failures

## 结果模板

| Date | Commit | Client | Connections | Mode | Payload | QPS | Avg | P50 | P99 | Tx | Rx | Failures |
| --- | --- | --- | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 2026-04-07 | example | python | 50 | duration=8s | 9 B | TBD | TBD | TBD | TBD | TBD | TBD | 0 |
| 2026-04-07 | example | cpp | 50 | duration=8s | 9 B | TBD | TBD | TBD | TBD | TBD | TBD | 0 |

## 说明

- 第一版 benchmark client 重点是提供一个稳定的项目内压测入口，而不是做完整性能分析平台。
- 后续可以在 C++ 版本继续补 `warmup`、`pipeline depth`、`histogram` 和 `CSV` 导出。

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
