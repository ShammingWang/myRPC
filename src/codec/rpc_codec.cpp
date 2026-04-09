#include "codec/rpc_codec.h"

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace {

void AppendUint16(std::string& out, uint16_t value) {
    out.push_back(static_cast<char>((value >> 8) & 0xff));
    out.push_back(static_cast<char>(value & 0xff));
}

void AppendUint32(std::string& out, uint32_t value) {
    out.push_back(static_cast<char>((value >> 24) & 0xff));
    out.push_back(static_cast<char>((value >> 16) & 0xff));
    out.push_back(static_cast<char>((value >> 8) & 0xff));
    out.push_back(static_cast<char>(value & 0xff));
}

void AppendUint64(std::string& out, uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<char>((value >> shift) & 0xff));
    }
}

uint16_t LoadUint16(const std::string& input, size_t offset) {
    return (static_cast<uint16_t>(static_cast<unsigned char>(input[offset])) << 8) |
           static_cast<uint16_t>(static_cast<unsigned char>(input[offset + 1]));
}

uint32_t LoadUint32(const std::string& input, size_t offset) {
    return (static_cast<uint32_t>(static_cast<unsigned char>(input[offset])) << 24) |
           (static_cast<uint32_t>(static_cast<unsigned char>(input[offset + 1])) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(input[offset + 2])) << 8) |
           static_cast<uint32_t>(static_cast<unsigned char>(input[offset + 3]));
}

uint64_t LoadUint64(const std::string& input, size_t offset) {
    uint64_t value = 0;
    for (size_t i = 0; i < sizeof(uint64_t); ++i) {
        value = (value << 8) | static_cast<unsigned char>(input[offset + i]);
    }
    return value;
}

}  // namespace

bool RpcCodec::TryDecodeRequest(std::string& buffer, RpcRequest& request, std::string& error) const {
    if (buffer.size() < kHeaderSize) {
        return false;
    }

    const uint32_t magic = LoadUint32(buffer, 0);
    const uint8_t version = static_cast<uint8_t>(buffer[4]);
    const uint8_t message_type = static_cast<uint8_t>(buffer[5]);
    const auto serialization = static_cast<RpcSerializationType>(
        static_cast<uint8_t>(buffer[6]));
    const uint32_t method_size = LoadUint32(buffer, 10);
    const uint32_t payload_size = LoadUint32(buffer, 14);
    const uint32_t timeout_ms = LoadUint32(buffer, 18);
    const uint64_t request_id = LoadUint64(buffer, 26);

    if (magic != kMagic) {
        error = "invalid magic";
        return false;
    }
    if (version != kVersion) {
        error = "unsupported version";
        return false;
    }
    if (message_type != kRequestType) {
        error = "unexpected message type";
        return false;
    }
    if (serialization != RpcSerializationType::kRaw &&
        serialization != RpcSerializationType::kJson) {
        error = "unsupported serialization type";
        return false;
    }
    if (method_size == 0 || method_size > kMaxMethodSize) {
        error = "invalid method size";
        return false;
    }
    if (payload_size > kMaxPayloadSize) {
        error = "payload too large";
        return false;
    }

    const size_t frame_size = kHeaderSize + static_cast<size_t>(method_size) +
                              static_cast<size_t>(payload_size);
    if (buffer.size() < frame_size) {
        return false;
    }

    request.request_id = request_id;
    request.serialization = serialization;
    request.timeout_ms = timeout_ms;
    request.received_at = std::chrono::steady_clock::now();
    request.method = buffer.substr(kHeaderSize, method_size);
    request.payload = buffer.substr(kHeaderSize + method_size, payload_size);
    buffer.erase(0, frame_size);
    error.clear();
    return true;
}

bool RpcCodec::TryDecodeResponse(std::string& buffer, RpcResponse& response, std::string& error) const {
    if (buffer.size() < kHeaderSize) {
        return false;
    }

    const uint32_t magic = LoadUint32(buffer, 0);
    const uint8_t version = static_cast<uint8_t>(buffer[4]);
    const uint8_t message_type = static_cast<uint8_t>(buffer[5]);
    const auto serialization = static_cast<RpcSerializationType>(
        static_cast<uint8_t>(buffer[6]));
    const uint16_t status_code = LoadUint16(buffer, 8);
    const uint32_t method_size = LoadUint32(buffer, 10);
    const uint32_t payload_size = LoadUint32(buffer, 14);
    const uint64_t request_id = LoadUint64(buffer, 26);

    if (magic != kMagic) {
        error = "invalid magic";
        return false;
    }
    if (version != kVersion) {
        error = "unsupported version";
        return false;
    }
    if (message_type != kResponseType) {
        error = "unexpected message type";
        return false;
    }
    if (serialization != RpcSerializationType::kRaw &&
        serialization != RpcSerializationType::kJson) {
        error = "unsupported serialization type";
        return false;
    }
    if (method_size != 0) {
        error = "unexpected response method size";
        return false;
    }
    if (payload_size > kMaxPayloadSize) {
        error = "payload too large";
        return false;
    }

    const size_t frame_size = kHeaderSize + static_cast<size_t>(payload_size);
    if (buffer.size() < frame_size) {
        return false;
    }

    response.request_id = request_id;
    response.status_code = static_cast<RpcStatusCode>(status_code);
    response.serialization = serialization;
    response.payload = buffer.substr(kHeaderSize, payload_size);
    buffer.erase(0, frame_size);
    error.clear();
    return true;
}

void RpcCodec::EncodeRequest(const RpcRequest& request, std::string& out) const {
    out.reserve(out.size() + kHeaderSize + request.method.size() + request.payload.size());
    AppendUint32(out, kMagic);
    out.push_back(static_cast<char>(kVersion));
    out.push_back(static_cast<char>(kRequestType));
    out.push_back(static_cast<char>(request.serialization));
    out.push_back(0);
    AppendUint16(out, 0);
    AppendUint32(out, static_cast<uint32_t>(request.method.size()));
    AppendUint32(out, static_cast<uint32_t>(request.payload.size()));
    AppendUint32(out, request.timeout_ms);
    AppendUint32(out, 0);
    AppendUint64(out, request.request_id);
    out.append(request.method);
    out.append(request.payload);
}

void RpcCodec::EncodeResponse(const RpcResponse& response, std::string& out) const {
    out.reserve(out.size() + kHeaderSize + response.payload.size());
    AppendUint32(out, kMagic);
    out.push_back(static_cast<char>(kVersion));
    out.push_back(static_cast<char>(kResponseType));
    out.push_back(static_cast<char>(response.serialization));
    out.push_back(0);
    AppendUint16(out, static_cast<uint16_t>(response.status_code));
    AppendUint32(out, 0);
    AppendUint32(out, static_cast<uint32_t>(response.payload.size()));
    AppendUint32(out, 0);
    AppendUint32(out, 0);
    AppendUint64(out, response.request_id);
    out.append(response.payload);
}
