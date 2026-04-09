#pragma once

#include <chrono>
#include <functional>
#include <string>

#include "codec/rpc_codec.h"
#include "codec/rpc_message.h"

struct ConnectionOptions {
    size_t max_pending_requests = 1024;
    size_t max_outbound_buffer_bytes = 1024 * 1024;
};

class Connection {
public:
    using RequestExecutor = std::function<void(RpcRequest)>;

    Connection(int conn_fd, std::string client_label, RequestExecutor request_executor,
               ConnectionOptions options = {});

    void OnConnected() const;
    bool OnReadable();
    bool OnWritable();
    bool WantsWrite() const;
    bool ShouldClose() const;
    bool IsIdleFor(std::chrono::steady_clock::time_point now,
                   std::chrono::milliseconds idle_timeout) const;
    void OnClosed() const;
    bool QueueResponse(const RpcResponse& response);

private:
    bool DrainReads();
    bool DrainWrites();
    bool ProcessRequests();
    void Touch();
    void QueueLocalError(uint64_t request_id, RpcStatusCode status_code, std::string message);

    int conn_fd_;
    std::string client_label_;
    RequestExecutor request_executor_;
    RpcCodec codec_;
    std::string inbound_buffer_;
    std::string outbound_buffer_;
    bool peer_closed_ = false;
    size_t pending_requests_ = 0;
    ConnectionOptions options_;
    std::chrono::steady_clock::time_point last_activity_;
};
