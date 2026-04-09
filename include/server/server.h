#pragma once

#include <chrono>
#include <csignal>
#include <cstdint>
#include <mutex>
#include <queue>
#include <string>
#include <unordered_map>

#include "codec/rpc_message.h"
#include "connection/connection.h"
#include "dispatcher/rpc_dispatcher.h"
#include "worker/worker_pool.h"

struct ServerOptions {
    std::chrono::milliseconds idle_connection_timeout{30000};
    size_t max_pending_requests_per_connection = 1024;
    size_t max_outbound_buffer_bytes_per_connection = 1024 * 1024;
};

class Server {
public:
    explicit Server(uint16_t port, ServerOptions options = {});

    void RegisterMethod(std::string service, std::string method, RpcDispatcher::MethodHandler handler);
    void RegisterService(const RpcService& service);
    bool Start();
    void Run();
    void Stop();

private:
    struct PendingResponse {
        int conn_fd = -1;
        RpcResponse response;
    };

    bool InitEpoll();
    void AcceptConnections();
    void CloseConnection(int conn_fd);
    bool UpdateInterest(int conn_fd, const Connection& connection);
    void DispatchRequest(int conn_fd, RpcRequest request);
    void EnqueueResponse(int conn_fd, RpcResponse response);
    bool DrainCompletedResponses();
    void CloseIdleConnections();
    RpcResponse BuildTimeoutResponse(const RpcRequest& request) const;

    uint16_t port_;
    int listen_fd_ = -1;
    int epoll_fd_ = -1;
    int wake_fd_ = -1;
    ServerOptions options_;
    RpcDispatcher dispatcher_;
    WorkerPool worker_pool_;
    std::unordered_map<int, Connection> connections_;
    std::mutex completed_mutex_;
    std::queue<PendingResponse> completed_responses_;
};

extern volatile std::sig_atomic_t g_running;
void HandleSignal(int);
