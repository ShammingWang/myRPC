# myRPC

这是一个面向 RPC 学习路线的练手项目。

当前阶段已经从“最基础的 TCP 网络通信”推进到了“更像基础组件的一版最小 RPC 框架”。

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
└── scripts/
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

项目内已经整理出一套 benchmark 目录，主压测入口是 C++ 版 client，Python 版脚本放在 `scripts/` 里作为补充工具：

```text
bench/
├── cpp_bench_client.cpp
├── run_bench.sh
├── cases/
└── results/
```

先启动服务：

```bash
./build/simple_tcp_server
```

### 编译 benchmark client

```bash
cmake -S . -B build
cmake --build build
```

C++ benchmark 可执行文件为：

```bash
./build/mrpc_bench_client
```

### 运行 benchmark

Python 版本：

```bash
python3 scripts/bench_python.py --connections 50 --requests 10000 --method EchoService.Echo --payload "hello rpc"
```

C++ 版本：

```bash
./build/mrpc_bench_client --connections 50 --requests 10000 --method EchoService.Echo --payload "hello rpc"
```

按固定时长压测：

```bash
./build/mrpc_bench_client --connections 20 --duration 30 --method EchoService.Uppercase --payload "hello" --expect-payload "HELLO"
```

两个版本当前都支持：

- `host / port / method / payload`
- `connections`
- `requests` 或 `duration`
- 连接复用，以及 `--reconnect-per-request`
- 输出 `QPS / avg / p50 / p99 / throughput / failures`

查看参数：

```bash
python3 scripts/bench_python.py --help
./build/mrpc_bench_client --help
```

### 如何看结果

输出里重点关注这些指标：

- `QPS`：每秒成功请求数
- `Latency avg / p50 / p99`：平均延迟与尾延迟
- `Tx / Rx throughput`：发送和接收吞吐
- `Failures`：失败请求数

更完整的记录模板见 [benchmark.md](/home/shamming/projects/myRPC/docs/benchmark.md)。

## Observability

服务端现在会输出三类观测日志：

- `/metrics`：独立 HTTP 管理端口导出当前连接、请求、错误、超时等指标
- `slow`：超过阈值的慢请求，包含 `queue_ms / handler_ms / io_return_ms / end_to_end_ms`
- `trace`：失败请求或开启全量 trace 时打印，带 `request_id / method / client / io_thread / connection_id`

## 最新性能结果

下面这组数据用于对比服务端从“单线程 IO”升级到“主线程 `accept` + 多 IO 线程”之后的收益。

测试口径：

- 日期：`2026-04-11`
- 测试机器：当前开发环境，`16 vCPU`
- 服务方法：`EchoService.Echo`
- payload：`hello rpc`，大小 `9 bytes`
- benchmark client：项目内 `mrpc_bench_client`
- 压测模式：固定时长 `duration=6s`
- 每组重复两次，表格内为平均值

对照组：

- `baseline single-io`：改造前基线版本 `bffdd75`
- `multi-io(1)`：新架构，但限制为 `1` 个 IO 线程
- `multi-io(16)`：新架构，使用 `16` 个 IO 线程

结果如下：

| Connections | Baseline QPS | Multi-io(1) QPS | Multi-io(16) QPS | Baseline Avg | Multi-io(16) Avg | Baseline P99 | Multi-io(16) P99 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 50 | 13793.31 | 13326.24 | 47594.32 | 3.619 ms | 1.046 ms | 6.209 ms | 3.338 ms |
| 100 | 13730.40 | 13531.55 | 48179.05 | 7.268 ms | 2.069 ms | 10.128 ms | 5.760 ms |
| 200 | 13347.12 | 13506.27 | 45729.07 | 14.945 ms | 4.358 ms | 19.569 ms | 10.558 ms |
| 400 | 13209.30 | 13446.99 | 43754.54 | 30.117 ms | 9.095 ms | 42.678 ms | 18.410 ms |

从这组结果可以看到：

- 新架构在 `io_thread_count=1` 时与旧版基本持平，说明重构本身没有明显额外开销。
- 当 `io_thread_count=16` 时，`QPS` 提升约 `2.3x ~ 2.5x`。
- 平均延迟下降约 `70%`，`P99` 下降约 `43% ~ 57%`。
- 对当前这种轻业务逻辑、高连接数场景，多 IO 线程模型明显优于单 IO 线程模型。

更完整的测试口径、完整对照表和收益汇总见 [benchmark.md](/home/shamming/projects/myRPC/docs/benchmark.md)。

## 下一步建议

1. 给 `RpcClient` 增加连接池、自动重连和异步调用接口。
2. 把 `json` serializer 从“轻校验”升级成真正的结构化序列化。
3. 继续把线程池细化成独立的 IO 线程池和业务线程池。
