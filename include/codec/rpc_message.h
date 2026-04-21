#pragma once

#include <chrono>
#include <cstdint>
#include <string>

enum class RpcSerializationType : uint8_t {
    kRaw = 0,
    kJson = 1,
};

enum class RpcStatusCode : uint16_t {
    kOk = 0,
    kMethodNotFound = 1001,
    kHandlerException = 1002,
    kInvalidRequest = 1003,
    kProtocolError = 1004,
    kSerializationError = 1005,
    kServerOverloaded = 1006,
    kRequestTimeout = 1007,
    kNetworkError = 2001,
    kClientTimeout = 2002,
};

struct RpcRequest {
    uint64_t request_id = 0;
    std::string method;
    std::string payload;
    RpcSerializationType serialization = RpcSerializationType::kRaw;
    uint32_t timeout_ms = 0;
    std::chrono::steady_clock::time_point received_at = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point enqueued_at = received_at;
    std::chrono::steady_clock::time_point worker_started_at = received_at;
    uint64_t connection_id = 0;
    size_t io_thread_index = 0;
    std::string client_label;
};

struct RpcResponse {
    uint64_t request_id = 0;
    RpcStatusCode status_code = RpcStatusCode::kOk;
    RpcSerializationType serialization = RpcSerializationType::kRaw;
    std::string payload;
};

inline bool IsSuccess(RpcStatusCode status_code) {
    return status_code == RpcStatusCode::kOk;
}

inline bool IsRequestTimedOut(const RpcRequest& request,
                              std::chrono::steady_clock::time_point now) {
    if (request.timeout_ms == 0) {
        return false;
    }
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - request.received_at);
    return elapsed.count() >= static_cast<int64_t>(request.timeout_ms);
}

inline std::string StatusCodeToString(RpcStatusCode status_code) {
    switch (status_code) {
        case RpcStatusCode::kOk:
            return "ok";
        case RpcStatusCode::kMethodNotFound:
            return "method not found";
        case RpcStatusCode::kHandlerException:
            return "handler exception";
        case RpcStatusCode::kInvalidRequest:
            return "invalid request";
        case RpcStatusCode::kProtocolError:
            return "protocol error";
        case RpcStatusCode::kSerializationError:
            return "serialization error";
        case RpcStatusCode::kServerOverloaded:
            return "server overloaded";
        case RpcStatusCode::kRequestTimeout:
            return "request timeout";
        case RpcStatusCode::kNetworkError:
            return "network error";
        case RpcStatusCode::kClientTimeout:
            return "client timeout";
    }

    return "unknown";
}
