# myRPC

这是一个面向 RPC 学习路线的练手项目。

当前阶段已经从“最基础的 TCP 网络通信”推进到了“最小 RPC 雏形”。

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
- 基于 `epoll` 的单线程 IO 多路复用
- 业务线程池异步执行 handler，IO 线程只负责网络收发和编解码
- 并发处理多个客户端连接
- 二进制长度字段帧协议
- `request_id` 关联请求和响应
- `Connection + Codec + Dispatcher` 三层结构
- 单连接内连续处理多条 RPC 请求
- 内置 `echo` 和 `uppercase` 两个示例方法

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

## 协议格式

请求和响应都使用固定 24 字节包头：

```text
4 bytes   magic      固定为 "MRPC"
1 byte    version    当前为 1
1 byte    type       1=request, 2=response
2 bytes   status     请求时固定为 0，响应时表示状态码
4 bytes   method_len 请求方法名长度，响应固定为 0
4 bytes   body_len   负载长度
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

可以用下面这段 Python 发两个请求到同一个连接，验证多请求复用：

```bash
python3 - <<'PY'
import socket
import struct

def pack_request(request_id, method, payload):
    method_b = method.encode()
    payload_b = payload.encode()
    header = struct.pack("!IBBHIIQ", 0x4D525043, 1, 1, 0, len(method_b), len(payload_b), request_id)
    return header + method_b + payload_b

def read_response(sock):
    header = sock.recv(24)
    magic, version, msg_type, status, method_len, body_len, request_id = struct.unpack("!IBBHIIQ", header)
    body = b""
    while len(body) < body_len:
        body += sock.recv(body_len - len(body))
    return {
        "magic": hex(magic),
        "version": version,
        "type": msg_type,
        "status": status,
        "request_id": request_id,
        "payload": body.decode(),
    }

with socket.create_connection(("127.0.0.1", 8080)) as sock:
    sock.sendall(pack_request(1, "echo", "hello rpc"))
    sock.sendall(pack_request(2, "uppercase", "hello again"))
    print(read_response(sock))
    print(read_response(sock))
PY
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
python3 scripts/bench_python.py --connections 50 --requests 10000 --method echo --payload "hello rpc"
```

C++ 版本：

```bash
./build/mrpc_bench_client --connections 50 --requests 10000 --method echo --payload "hello rpc"
```

按固定时长压测：

```bash
./build/mrpc_bench_client --connections 20 --duration 30 --method uppercase --payload "hello" --expect-payload "HELLO"
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

## 性能基线

下面这组数据是在 `feature/worker-pool` 分支上测得的一个阶段性基线，用于后续版本横向对比。

测试口径：

- 服务端为当前 `WorkerPool` 版本
- 临时关闭了请求级日志，只保留连接级日志
- 本机回环 `127.0.0.1:8080`
- `echo` 方法
- payload 固定为 `hello rpc`，大小 `9 bytes`
- 连接复用，不开启 `--reconnect-per-request`
- 每组压测时长 `8s`
- 测试环境：`WSL2 / 16 vCPU`

测试命令模板：

```bash
python3 scripts/bench_python.py --connections <N> --duration 8 --method echo --payload "hello rpc" --expect-payload "hello rpc"
```

结果如下：

| Connections | QPS | Avg Latency | P50 | P90 | P99 | Max |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 10 | 3471.73 | 2.871 ms | 2.768 ms | 4.109 ms | 5.541 ms | 8.647 ms |
| 20 | 3522.24 | 5.661 ms | 5.441 ms | 8.306 ms | 11.310 ms | 19.088 ms |
| 50 | 3592.93 | 13.812 ms | 13.485 ms | 20.563 ms | 28.096 ms | 47.611 ms |
| 100 | 3581.94 | 27.400 ms | 27.085 ms | 41.111 ms | 56.377 ms | 129.330 ms |
| 200 | 4208.42 | 43.817 ms | 40.924 ms | 76.902 ms | 118.470 ms | 237.007 ms |

从这组结果可以看到：

- `10` 到 `100` 连接之间，吞吐基本稳定在 `3.5k QPS` 左右
- `200` 连接时吞吐提升到 `4.2k QPS`，但尾延迟明显上升
- 如果更关注延迟，`50` 或 `100` 连接更均衡
- 如果更关注吞吐上探，`200` 连接可以作为当前版本的极限参考点

## 下一步建议

1. 把 `Dispatcher` 从“方法名到函数”升级成真正的服务注册中心。
2. 给 `Connection` 增加写缓冲上限、请求超时和空闲连接清理。
3. 继续把线程池细化成独立的 IO 线程池和业务线程池。
