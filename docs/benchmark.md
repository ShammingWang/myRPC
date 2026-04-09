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
