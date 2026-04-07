#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

constexpr uint32_t kMagic = 0x4D525043;
constexpr uint8_t kVersion = 1;
constexpr uint8_t kRequestType = 1;
constexpr uint8_t kResponseType = 2;
constexpr size_t kHeaderSize = 24;

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

uint32_t LoadUint32(const char* data) {
    return (static_cast<uint32_t>(static_cast<unsigned char>(data[0])) << 24) |
           (static_cast<uint32_t>(static_cast<unsigned char>(data[1])) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(data[2])) << 8) |
           static_cast<uint32_t>(static_cast<unsigned char>(data[3]));
}

uint16_t LoadUint16(const char* data) {
    return (static_cast<uint16_t>(static_cast<unsigned char>(data[0])) << 8) |
           static_cast<uint16_t>(static_cast<unsigned char>(data[1]));
}

uint64_t LoadUint64(const char* data) {
    uint64_t value = 0;
    for (size_t i = 0; i < sizeof(uint64_t); ++i) {
        value = (value << 8) | static_cast<unsigned char>(data[i]);
    }
    return value;
}

std::string BuildRequest(uint64_t request_id, const std::string& method, const std::string& payload) {
    std::string packet;
    packet.reserve(kHeaderSize + method.size() + payload.size());
    AppendUint32(packet, kMagic);
    packet.push_back(static_cast<char>(kVersion));
    packet.push_back(static_cast<char>(kRequestType));
    AppendUint16(packet, 0);
    AppendUint32(packet, static_cast<uint32_t>(method.size()));
    AppendUint32(packet, static_cast<uint32_t>(payload.size()));
    AppendUint64(packet, request_id);
    packet.append(method);
    packet.append(payload);
    return packet;
}

void SendAll(int fd, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, 0);
        if (n <= 0) {
            throw std::runtime_error("send failed");
        }
        sent += static_cast<size_t>(n);
    }
}

void RecvExact(int fd, char* buffer, size_t length) {
    size_t received = 0;
    while (received < length) {
        const ssize_t n = ::recv(fd, buffer + received, length - received, 0);
        if (n <= 0) {
            throw std::runtime_error("recv failed");
        }
        received += static_cast<size_t>(n);
    }
}

}  // namespace

int main() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        std::cerr << "socket failed\n";
        return 1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8080);
    if (::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
        std::cerr << "inet_pton failed\n";
        ::close(fd);
        return 1;
    }

    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::cerr << "connect failed: " << std::strerror(errno) << "\n";
        ::close(fd);
        return 1;
    }

    try {
        const std::string request = BuildRequest(1, "echo", "hello rpc");
        SendAll(fd, request);

        char header[kHeaderSize];
        RecvExact(fd, header, sizeof(header));
        const uint32_t magic = LoadUint32(header);
        const uint8_t version = static_cast<uint8_t>(header[4]);
        const uint8_t type = static_cast<uint8_t>(header[5]);
        const uint16_t status = LoadUint16(header + 6);
        const uint32_t body_len = LoadUint32(header + 12);
        const uint64_t request_id = LoadUint64(header + 16);

        std::string body(body_len, '\0');
        if (body_len > 0) {
            RecvExact(fd, body.data(), body_len);
        }

        std::cout << "magic=" << std::hex << magic << std::dec
                  << " version=" << static_cast<int>(version)
                  << " type=" << static_cast<int>(type)
                  << " status=" << status
                  << " request_id=" << request_id
                  << " payload=" << body << "\n";
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << "\n";
        ::close(fd);
        return 1;
    }

    ::close(fd);
    return 0;
}
