#include "client/rpc_client.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "codec/rpc_serialization.h"

namespace {

bool SendAll(int fd, const std::string& data, std::string& error) {
    size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            error = std::string("send failed: ") + std::strerror(errno);
            return false;
        }
        sent += static_cast<size_t>(n);
    }

    error.clear();
    return true;
}

}  // namespace

RpcClient::RpcClient(RpcClientOptions options) : options_(std::move(options)) {}

RpcClient::~RpcClient() {
    Close();
}

bool RpcClient::Connect(std::string& error) {
    std::lock_guard<std::mutex> lock(connection_mutex_);
    if (running_) {
        error.clear();
        return true;
    }

    return ConnectSocket(error);
}

void RpcClient::Close() {
    {
        std::lock_guard<std::mutex> lock(connection_mutex_);
        running_ = false;
        if (sock_fd_ >= 0) {
            ::shutdown(sock_fd_, SHUT_RDWR);
            ::close(sock_fd_);
            sock_fd_ = -1;
        }
    }

    if (reader_thread_.joinable()) {
        reader_thread_.join();
    }

    FailAllPending("client closed");
}

RpcResponse RpcClient::Call(std::string method, std::string payload, RpcCallOptions options) {
    RpcResponse failure;
    failure.serialization = options.serialization;

    std::string error;
    if (!EnsureConnected(error)) {
        failure.status_code = RpcStatusCode::kNetworkError;
        failure.payload = error;
        return failure;
    }

    std::string encoded_payload;
    if (!RpcSerializerRegistry::Serialize(options.serialization, payload, encoded_payload, error)) {
        failure.status_code = RpcStatusCode::kSerializationError;
        failure.payload = error;
        return failure;
    }

    RpcRequest request;
    request.request_id = next_request_id_.fetch_add(1);
    request.method = std::move(method);
    request.payload = std::move(encoded_payload);
    request.serialization = options.serialization;
    request.timeout_ms = options.timeout_ms;

    auto pending = std::make_shared<PendingCall>();
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_calls_.emplace(request.request_id, pending);
    }

    std::string packet;
    codec_.EncodeRequest(request, packet);
    if (!SendPacket(packet, error)) {
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            pending_calls_.erase(request.request_id);
        }
        failure.request_id = request.request_id;
        failure.status_code = RpcStatusCode::kNetworkError;
        failure.payload = error;
        return failure;
    }

    const uint32_t wait_timeout_ms =
        options.wait_timeout_ms == 0
            ? (options.timeout_ms == 0 ? 1000 : options.timeout_ms + 200)
            : options.wait_timeout_ms;
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(wait_timeout_ms);
    std::unique_lock<std::mutex> lock(pending->mutex);
    if (!pending->cv.wait_until(lock, deadline, [&pending] { return pending->done; })) {
        {
            std::lock_guard<std::mutex> pending_lock(pending_mutex_);
            pending_calls_.erase(request.request_id);
        }
        failure.request_id = request.request_id;
        failure.status_code = RpcStatusCode::kClientTimeout;
        failure.payload = "client wait for response timed out";
        return failure;
    }

    if (!pending->error.empty()) {
        failure.request_id = request.request_id;
        failure.status_code = RpcStatusCode::kNetworkError;
        failure.payload = pending->error;
        return failure;
    }

    return pending->response;
}

RpcResponse RpcClient::Call(std::string service, std::string method, std::string payload,
                            RpcCallOptions options) {
    return Call(BuildMethodName(service, method), std::move(payload), options);
}

bool RpcClient::EnsureConnected(std::string& error) {
    if (running_) {
        error.clear();
        return true;
    }
    return Connect(error);
}

bool RpcClient::ConnectSocket(std::string& error) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* result = nullptr;
    const std::string port = std::to_string(options_.port);
    const int rc = ::getaddrinfo(options_.host.c_str(), port.c_str(), &hints, &result);
    if (rc != 0) {
        error = std::string("getaddrinfo failed: ") + gai_strerror(rc);
        return false;
    }

    int connected_fd = -1;
    for (addrinfo* current = result; current != nullptr; current = current->ai_next) {
        const int fd = ::socket(current->ai_family, current->ai_socktype, current->ai_protocol);
        if (fd < 0) {
            continue;
        }

        timeval timeout{};
        timeout.tv_sec = static_cast<long>(options_.connect_timeout_ms / 1000);
        timeout.tv_usec = static_cast<long>((options_.connect_timeout_ms % 1000) * 1000);
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

        if (::connect(fd, current->ai_addr, current->ai_addrlen) == 0) {
            connected_fd = fd;
            break;
        }

        ::close(fd);
    }

    ::freeaddrinfo(result);

    if (connected_fd < 0) {
        error = "failed to connect to RPC server";
        return false;
    }

    sock_fd_ = connected_fd;
    running_ = true;
    reader_thread_ = std::thread(&RpcClient::ReaderLoop, this);
    error.clear();
    return true;
}

void RpcClient::ReaderLoop() {
    while (running_) {
        char buffer[4096];
        const ssize_t n = ::recv(sock_fd_, buffer, sizeof(buffer), 0);
        if (n == 0) {
            FailAllPending("server closed connection");
            break;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            FailAllPending(std::string("recv failed: ") + std::strerror(errno));
            break;
        }

        read_buffer_.append(buffer, static_cast<size_t>(n));
        while (true) {
            RpcResponse response;
            std::string error;
            if (!codec_.TryDecodeResponse(read_buffer_, response, error)) {
                if (!error.empty()) {
                    FailAllPending(error);
                    running_ = false;
                }
                break;
            }

            std::shared_ptr<PendingCall> pending;
            {
                std::lock_guard<std::mutex> lock(pending_mutex_);
                const auto it = pending_calls_.find(response.request_id);
                if (it == pending_calls_.end()) {
                    continue;
                }
                pending = it->second;
                pending_calls_.erase(it);
            }

            {
                std::lock_guard<std::mutex> lock(pending->mutex);
                pending->response = std::move(response);
                pending->done = true;
            }
            pending->cv.notify_one();
        }
    }

    running_ = false;
}

bool RpcClient::SendPacket(const std::string& packet, std::string& error) {
    std::lock_guard<std::mutex> lock(send_mutex_);
    if (sock_fd_ < 0) {
        error = "client is not connected";
        return false;
    }
    return SendAll(sock_fd_, packet, error);
}

void RpcClient::FailAllPending(const std::string& error) {
    std::unordered_map<uint64_t, std::shared_ptr<PendingCall>> pending;
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending.swap(pending_calls_);
    }

    for (auto& [_, call] : pending) {
        {
            std::lock_guard<std::mutex> lock(call->mutex);
            if (call->done) {
                continue;
            }
            call->done = true;
            call->error = error;
        }
        call->cv.notify_one();
    }
}

std::string RpcClient::BuildMethodName(const std::string& service, const std::string& method) {
    if (service.empty()) {
        return method;
    }
    return service + "." + method;
}
