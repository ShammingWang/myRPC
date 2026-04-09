#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>

#include "codec/rpc_codec.h"
#include "codec/rpc_message.h"

struct RpcClientOptions {
    std::string host = "127.0.0.1";
    uint16_t port = 8080;
    uint32_t connect_timeout_ms = 3000;
};

struct RpcCallOptions {
    uint32_t timeout_ms = 1000;
    uint32_t wait_timeout_ms = 0;
    RpcSerializationType serialization = RpcSerializationType::kRaw;
};

class RpcClient {
public:
    explicit RpcClient(RpcClientOptions options = {});
    ~RpcClient();

    RpcClient(const RpcClient&) = delete;
    RpcClient& operator=(const RpcClient&) = delete;

    bool Connect(std::string& error);
    void Close();

    RpcResponse Call(std::string method, std::string payload,
                     RpcCallOptions options = {});
    RpcResponse Call(std::string service, std::string method, std::string payload,
                     RpcCallOptions options = {});

private:
    struct PendingCall {
        std::mutex mutex;
        std::condition_variable cv;
        bool done = false;
        RpcResponse response;
        std::string error;
    };

    bool EnsureConnected(std::string& error);
    bool ConnectSocket(std::string& error);
    void ReaderLoop();
    bool SendPacket(const std::string& packet, std::string& error);
    void FailAllPending(const std::string& error);
    static std::string BuildMethodName(const std::string& service, const std::string& method);

    RpcClientOptions options_;
    int sock_fd_ = -1;
    std::atomic<bool> running_{false};
    std::thread reader_thread_;
    std::mutex connection_mutex_;
    std::mutex send_mutex_;
    std::mutex pending_mutex_;
    std::unordered_map<uint64_t, std::shared_ptr<PendingCall>> pending_calls_;
    std::atomic<uint64_t> next_request_id_{1};
    RpcCodec codec_;
    std::string read_buffer_;
};
