#include "connection/connection.h"

#include <cerrno>
#include <cstring>
#include <iostream>
#include <utility>
#include <unistd.h>

namespace {

constexpr size_t kBufferSize = 4096;
std::mutex g_connection_log_mutex;

}  // namespace

Connection::Connection(int conn_fd, std::string client_label, RequestExecutor request_executor,
                       ConnectionOptions options)
    : conn_fd_(conn_fd),
      client_label_(std::move(client_label)),
      request_executor_(std::move(request_executor)),
      options_(options),
      last_activity_(std::chrono::steady_clock::now()) {}

void Connection::OnConnected() const {
    {
        std::lock_guard<std::mutex> lock(g_connection_log_mutex);
        std::cout << "accepted connection from " << client_label_ << '\n';
    }
    if (options_.observability) {
        options_.observability->OnConnectionAccepted();
    }
}

bool Connection::OnReadable() {
    Touch();
    if (!DrainReads()) {
        return false;
    }

    return ProcessRequests();
}

bool Connection::OnWritable() {
    Touch();
    return DrainWrites();
}

bool Connection::WantsWrite() const {
    return !outbound_buffer_.empty();
}

bool Connection::ShouldClose() const {
    return peer_closed_ && outbound_buffer_.empty() && pending_requests_ == 0;
}

bool Connection::IsIdleFor(std::chrono::steady_clock::time_point now,
                           std::chrono::milliseconds idle_timeout) const {
    if (pending_requests_ > 0 || !outbound_buffer_.empty()) {
        return false;
    }
    return now - last_activity_ >= idle_timeout;
}

void Connection::OnClosed() const {
    {
        std::lock_guard<std::mutex> lock(g_connection_log_mutex);
        std::cout << "connection closed: " << client_label_ << '\n';
    }
    if (options_.observability) {
        options_.observability->OnConnectionClosed();
    }
}

bool Connection::QueueResponse(const RpcResponse& response) {
    std::string encoded;
    codec_.EncodeResponse(response, encoded);
    if (outbound_buffer_.size() + encoded.size() > options_.max_outbound_buffer_bytes) {
        std::cerr << "outbound buffer exceeded for " << client_label_ << '\n';
        return false;
    }

    outbound_buffer_.append(encoded);
    if (pending_requests_ > 0) {
        --pending_requests_;
    }
    Touch();
    return true;
}

bool Connection::DrainReads() {
    while (true) {
        char buffer[kBufferSize];
        const ssize_t n = ::read(conn_fd_, buffer, sizeof(buffer));
        if (n == 0) {
            peer_closed_ = true;
            Touch();
            return true;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return true;
            }
            std::cerr << "read failed for " << client_label_ << ": " << std::strerror(errno)
                      << '\n';
            return false;
        }

        inbound_buffer_.append(buffer, static_cast<size_t>(n));
        Touch();
    }
}

bool Connection::DrainWrites() {
    size_t written = 0;
    while (written < outbound_buffer_.size()) {
        const ssize_t n =
            ::write(conn_fd_, outbound_buffer_.data() + written, outbound_buffer_.size() - written);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                outbound_buffer_.erase(0, written);
                return true;
            }
            std::cerr << "write failed for " << client_label_ << ": " << std::strerror(errno)
                      << '\n';
            return false;
        }

        written += static_cast<size_t>(n);
        Touch();
    }

    outbound_buffer_.clear();
    return true;
}

bool Connection::ProcessRequests() {
    while (true) {
        RpcRequest request;
        std::string error;
        if (!codec_.TryDecodeRequest(inbound_buffer_, request, error)) {
            if (!error.empty()) {
                std::cerr << "decode failed for " << client_label_ << ": " << error << '\n';
                if (options_.observability) {
                    options_.observability->OnProtocolError();
                }
                return false;
            }
            return true;
        }

        request.client_label = client_label_;
        request.connection_id = options_.connection_id;
        request.io_thread_index = options_.io_thread_index;
        if (pending_requests_ >= options_.max_pending_requests) {
            QueueLocalError(request.request_id, RpcStatusCode::kServerOverloaded,
                            "too many inflight requests on this connection");
            continue;
        }

        ++pending_requests_;
        if (options_.observability) {
            options_.observability->OnRequestDecoded();
        }
        request_executor_(std::move(request));
    }
}

void Connection::Touch() {
    last_activity_ = std::chrono::steady_clock::now();
}

void Connection::QueueLocalError(uint64_t request_id, RpcStatusCode status_code, std::string message) {
    RpcResponse response;
    response.request_id = request_id;
    response.status_code = status_code;
    response.serialization = RpcSerializationType::kRaw;
    response.payload = std::move(message);

    std::string encoded;
    codec_.EncodeResponse(response, encoded);
    if (outbound_buffer_.size() + encoded.size() <= options_.max_outbound_buffer_bytes) {
        outbound_buffer_.append(encoded);
    } else {
        peer_closed_ = true;
    }
    Touch();
}
