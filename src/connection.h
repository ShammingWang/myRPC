#pragma once

#include <functional>
#include <string>

#include "rpc_codec.h"
#include "rpc_message.h"

class Connection {
public:
    using RequestExecutor = std::function<void(RpcRequest)>;

    Connection(int conn_fd, std::string client_label, RequestExecutor request_executor);

    void OnConnected() const;
    bool OnReadable();
    bool OnWritable();
    bool WantsWrite() const;
    bool ShouldClose() const;
    void OnClosed() const;
    void QueueResponse(const RpcResponse& response);

private:
    bool DrainReads();
    bool DrainWrites();
    bool ProcessRequests();

    int conn_fd_;
    std::string client_label_;
    RequestExecutor request_executor_;
    RpcCodec codec_;
    std::string inbound_buffer_;
    std::string outbound_buffer_;
    bool peer_closed_ = false;
    size_t pending_requests_ = 0;
};
