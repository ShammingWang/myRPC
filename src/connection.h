#pragma once

#include <string>

#include "rpc_codec.h"
#include "rpc_dispatcher.h"

class Connection {
public:
    Connection(int conn_fd, std::string client_label, const RpcDispatcher& dispatcher);

    void OnConnected() const;
    bool OnReadable();
    bool OnWritable();
    bool WantsWrite() const;
    bool ShouldClose() const;
    void OnClosed() const;

private:
    bool DrainReads();
    bool DrainWrites();
    bool ProcessRequests();

    int conn_fd_;
    std::string client_label_;
    const RpcDispatcher& dispatcher_;
    RpcCodec codec_;
    std::string inbound_buffer_;
    std::string outbound_buffer_;
    bool peer_closed_ = false;
};
