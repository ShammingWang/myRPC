# Architecture

当前 `myRPC` 已经具备“像基础组件”的最小闭环，核心链路由 `Acceptor + IO Threads + Connection + Codec + Dispatcher + WorkerPool + RpcClient` 组成。

## 线程模型

当前运行时包含三类线程：

- 主线程
  负责监听端口、执行 `accept`，并将新连接按 round-robin 分发给某个 IO 线程。
- IO 线程
  每个 IO 线程拥有自己的 `epoll` 实例，负责连接读写、编解码、响应发送和连接生命周期管理。
- WorkerPool 线程
  负责业务 handler 执行，避免业务逻辑阻塞 IO 线程。

这种模型的核心目标是把“网络事件处理”和“业务执行”拆开，同时保留连接与响应回写的线程亲和性。

## 分层

- `src/server/`
  负责监听端口、主线程 `accept`、多 IO 线程事件循环、响应回投、管理端口和连接生命周期管理。
- `src/connection/`
  维护单个 TCP 连接的读缓冲、写缓冲、请求解析和连接关闭状态。
- `src/codec/`
  负责 MRPC 二进制协议的请求解码和响应编码，以及序列化元数据处理。
- `src/dispatcher/`
  提供 `Service / Method` 注册与分发能力。
- `src/worker/`
  提供业务执行线程池，避免 handler 阻塞 IO 线程。
- `src/client/`
  提供正式 RPC client，负责连接管理、请求发送、响应匹配与超时控制。
- `src/observability/`
  提供 metrics、slow log 和 request trace，帮助做性能定位与问题排查。

## 请求流转

1. 主线程接收客户端连接，并按 round-robin 分发给某个 IO 线程。
2. IO 线程将连接注册到自己的 `epoll` 实例。
3. `Connection` 从 socket 读取字节流并累积到读缓冲。
4. `RpcCodec` 从缓冲中解析出完整的 `RpcRequest`。
5. `Server` 将请求投递到 `WorkerPool`。
6. `RpcDispatcher` 按 `Service.Method` 查找并执行 handler。
7. 业务线程产出 `RpcResponse` 后，经目标 IO 线程的 `eventfd` 唤醒对应 reactor。
8. `Connection` 将响应编码后写回客户端。
9. `Observability` 汇总请求队列时间、handler 执行时间、响应回投时间和端到端时延，并按条件输出 `slow / trace`，同时通过独立 HTTP 管理端口暴露 `/metrics`。

## Observability 接入点

当前观测数据主要在下面这些节点采集：

- 连接建立和关闭
  统计连接接入数与当前活跃连接数。
- 请求解码成功
  统计总请求数和 in-flight 请求数。
- worker pool 拒绝
  统计过载与投递失败场景。
- 响应发送完成
  汇总 `queue_ms / handler_ms / io_return_ms / end_to_end_ms`，并判断是否输出 `slow` 或 `trace`。
- 管理端口 `/metrics`
  通过独立 HTTP 端口暴露当前快照，便于 benchmark 和问题排查时统一抓取。

## 当前特征

- 主线程 `accept`，多 IO 线程处理网络事件，多线程业务执行
- 单连接支持连续多次 RPC 请求
- 自定义二进制协议，靠 `request_id` 关联请求与响应
- 连接级背压：限制 in-flight 请求数与写缓冲大小
- 服务端支持空闲连接清理和请求超时处理
- 客户端支持同步调用、响应匹配和超时等待
- 服务端内建 `/metrics`、slow log 和 request trace
- 当前仍是单节点运行时，不包含注册中心、服务发现和治理能力

## 当前边界

- 当前 benchmark 主要覆盖本机回环网络，更适合观察框架内部路径的变化。
- 客户端目前以同步调用模型为主，还没有连接池和异步 outstanding requests。
- 当前 payload 与 handler 场景仍偏轻量，后续可以继续增加更重业务路径来观察瓶颈迁移。
