#pragma once

#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <string>

#include "codec/rpc_codec.h"
#include "codec/rpc_message.h"
#include "observability/observability.h"

struct ConnectionOptions {
    size_t max_pending_requests = 1024;
    size_t max_outbound_buffer_bytes = 1024 * 1024;
    size_t io_thread_index = 0;
    uint64_t connection_id = 0;
    std::shared_ptr<Observability> observability;
};

class Connection {
public:
    using RequestExecutor = std::function<void(RpcRequest)>;
    struct ResponseContext {
        RpcRequest request;
        RpcResponse response;
        std::chrono::steady_clock::time_point worker_finished_at =
            std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point response_enqueued_at =
            std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point io_processing_started_at =
            std::chrono::steady_clock::now();
    };

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
    bool QueueResponse(ResponseContext response_context);

private:
    struct BufferedResponse {
        ResponseContext context;
        size_t end_offset = 0;
    };

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
    std::deque<BufferedResponse> buffered_responses_;
    bool peer_closed_ = false;
    size_t pending_requests_ = 0;
    size_t outbound_bytes_sent_ = 0;
    ConnectionOptions options_;
    std::chrono::steady_clock::time_point last_activity_;
};
