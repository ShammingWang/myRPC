# Architecture

当前 `myRPC` 的服务端是一个最小可用的单进程 RPC Runtime，核心链路由 `Server + Connection + Codec + Dispatcher + WorkerPool` 组成。

## 分层

- `src/server/`
  负责监听端口、`epoll` 事件循环、连接接入、响应回投和生命周期管理。
- `src/connection/`
  维护单个 TCP 连接的读缓冲、写缓冲、请求解析和连接关闭状态。
- `src/codec/`
  负责 MRPC 二进制协议的请求解码和响应编码。
- `src/dispatcher/`
  提供 `method -> handler` 的分发能力。
- `src/worker/`
  提供业务执行线程池，避免 handler 阻塞 IO 线程。

## 请求流转

1. 客户端连接进入 `Server` 的 `epoll` 主循环。
2. `Connection` 从 socket 读取字节流并累积到读缓冲。
3. `RpcCodec` 从缓冲中解析出完整的 `RpcRequest`。
4. `Server` 将请求投递到 `WorkerPool`。
5. `RpcDispatcher` 按方法名查找并执行 handler。
6. 业务线程产出 `RpcResponse` 后，经 `eventfd` 唤醒 IO 线程。
7. `Connection` 将响应编码后写回客户端。

## 当前特征

- 单线程 IO，多线程业务执行
- 单连接支持连续多次 RPC 请求
- 自定义二进制协议，靠 `request_id` 关联请求与响应
- 当前仍是单节点运行时，不包含注册中心、服务发现和治理能力
