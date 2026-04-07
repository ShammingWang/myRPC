#pragma once

#include <cstdint>
#include <string>

#include "codec/rpc_message.h"

class RpcCodec {
public:
    bool TryDecodeRequest(std::string& buffer, RpcRequest& request, std::string& error) const;
    void EncodeResponse(const RpcResponse& response, std::string& out) const;

private:
    static constexpr uint32_t kMagic = 0x4d525043;  // MRPC
    static constexpr uint8_t kVersion = 1;
    static constexpr uint8_t kRequestType = 1;
    static constexpr uint8_t kResponseType = 2;
    static constexpr size_t kHeaderSize = 24;
    static constexpr uint32_t kMaxMethodSize = 1024;
    static constexpr uint32_t kMaxPayloadSize = 1024 * 1024;
};
