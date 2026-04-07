#pragma once

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

class Server {
public:
    explicit Server(uint16_t port);

    void RegisterHandler(std::string method, RpcDispatcher::Handler handler);
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

    uint16_t port_;
    int listen_fd_ = -1;
    int epoll_fd_ = -1;
    int wake_fd_ = -1;
    RpcDispatcher dispatcher_;
    WorkerPool worker_pool_;
    std::unordered_map<int, Connection> connections_;
    std::mutex completed_mutex_;
    std::queue<PendingResponse> completed_responses_;
};

extern volatile std::sig_atomic_t g_running;
void HandleSignal(int);
