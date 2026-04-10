#pragma once

#include <chrono>
#include <csignal>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "codec/rpc_message.h"
#include "connection/connection.h"
#include "dispatcher/rpc_dispatcher.h"
#include "worker/worker_pool.h"

struct ServerOptions {
    std::chrono::milliseconds idle_connection_timeout{30000};
    size_t max_pending_requests_per_connection = 1024;
    size_t max_outbound_buffer_bytes_per_connection = 1024 * 1024;
    size_t io_thread_count = 0;
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
    struct AcceptedConnection {
        int conn_fd = -1;
        uint64_t connection_id = 0;
        std::string client_label;
    };

    struct PendingResponse {
        size_t io_thread_index = 0;
        int conn_fd = -1;
        uint64_t connection_id = 0;
        RpcResponse response;
    };

    struct ConnectionEntry {
        uint64_t connection_id = 0;
        Connection connection;
    };

    struct IoThreadState {
        size_t index = 0;
        int epoll_fd = -1;
        int wake_fd = -1;
        std::thread thread;
        std::mutex mutex;
        std::queue<AcceptedConnection> accepted_connections;
        std::queue<PendingResponse> completed_responses;
        std::unordered_map<int, ConnectionEntry> connections;
    };

    bool InitAcceptorEpoll();
    bool StartIoThreads();
    bool InitIoThread(IoThreadState& io_thread);
    void RunIoThread(size_t io_thread_index);
    void AcceptConnections();
    void EnqueueAcceptedConnection(size_t io_thread_index, AcceptedConnection connection);
    void CloseConnection(IoThreadState& io_thread, int conn_fd);
    bool UpdateInterest(IoThreadState& io_thread, int conn_fd, const Connection& connection);
    void DispatchRequest(size_t io_thread_index, int conn_fd, uint64_t connection_id,
                         RpcRequest request);
    void EnqueueResponse(size_t io_thread_index, int conn_fd, uint64_t connection_id,
                         RpcResponse response);
    bool DrainIoThreadQueues(IoThreadState& io_thread);
    void DrainAcceptedConnections(IoThreadState& io_thread,
                                  std::queue<AcceptedConnection>& accepted_connections);
    void DrainCompletedResponses(IoThreadState& io_thread,
                                 std::queue<PendingResponse>& completed_responses);
    void CloseIdleConnections(IoThreadState& io_thread);
    void WakeIoThread(IoThreadState& io_thread) const;
    RpcResponse BuildTimeoutResponse(const RpcRequest& request) const;

    uint16_t port_;
    int listen_fd_ = -1;
    int epoll_fd_ = -1;
    ServerOptions options_;
    RpcDispatcher dispatcher_;
    WorkerPool worker_pool_;
    std::vector<std::unique_ptr<IoThreadState>> io_threads_;
    std::atomic<size_t> next_io_thread_{0};
    std::atomic<uint64_t> next_connection_id_{1};
    std::atomic<bool> stopping_{false};
};

extern volatile std::sig_atomic_t g_running;
void HandleSignal(int);
