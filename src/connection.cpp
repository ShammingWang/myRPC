#include "connection.h"

#include <cerrno>
#include <cstring>
#include <iostream>
#include <unistd.h>

namespace {

constexpr size_t kBufferSize = 4096;

}  // namespace

Connection::Connection(int conn_fd, std::string client_label, RequestExecutor request_executor)
    : conn_fd_(conn_fd),
      client_label_(std::move(client_label)),
      request_executor_(std::move(request_executor)) {}

void Connection::OnConnected() const {
    std::cout << "accepted connection from " << client_label_ << '\n';
}

bool Connection::OnReadable() {
    if (!DrainReads()) {
        return false;
    }

    return ProcessRequests();
}

bool Connection::OnWritable() {
    return DrainWrites();
}

bool Connection::WantsWrite() const {
    return !outbound_buffer_.empty();
}

bool Connection::ShouldClose() const {
    return peer_closed_ && outbound_buffer_.empty() && pending_requests_ == 0;
}

void Connection::OnClosed() const {
    std::cout << "connection closed: " << client_label_ << '\n';
}

void Connection::QueueResponse(const RpcResponse& response) {
    codec_.EncodeResponse(response, outbound_buffer_);
    if (pending_requests_ > 0) {
        --pending_requests_;
    }
}

bool Connection::DrainReads() {
    while (true) {
        char buffer[kBufferSize];
        const ssize_t n = ::read(conn_fd_, buffer, sizeof(buffer));
        if (n == 0) {
            peer_closed_ = true;
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
                return false;
            }
            return true;
        }

        ++pending_requests_;
        request_executor_(std::move(request));
    }
}
