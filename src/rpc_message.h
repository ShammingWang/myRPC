#pragma once

#include <cstdint>
#include <string>

struct RpcRequest {
    uint64_t request_id = 0;
    std::string method;
    std::string payload;
};

struct RpcResponse {
    uint64_t request_id = 0;
    uint16_t status_code = 0;
    std::string payload;
};
