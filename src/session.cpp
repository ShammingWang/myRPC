#include "session.h"

#include <cerrno>
#include <cstring>
#include <iostream>
#include <unistd.h>

namespace {

constexpr size_t kBufferSize = 4096;

}  // namespace

Session::Session(int conn_fd, std::string client_label)
    : conn_fd_(conn_fd), client_label_(std::move(client_label)) {}

void Session::OnConnected() const {
    std::cout << "accepted connection from " << client_label_ << '\n';
}

bool Session::OnReadable() {
    if (!DrainReads()) {
        return false;
    }

    ProcessRequests();
    return true;
}

bool Session::OnWritable() {
    return DrainWrites();
}

bool Session::WantsWrite() const {
    return !pending_write_.empty();
}

bool Session::ShouldClose() const {
    return peer_closed_ && pending_write_.empty();
}

void Session::OnClosed() const {
    std::cout << "connection closed: " << client_label_ << '\n';
}

bool Session::DrainReads() {
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

        pending_data_.append(buffer, static_cast<size_t>(n));
    }
}

bool Session::DrainWrites() {
    size_t written = 0;
    while (written < pending_write_.size()) {
        const ssize_t n =
            ::write(conn_fd_, pending_write_.data() + written, pending_write_.size() - written);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                pending_write_.erase(0, written);
                return true;
            }
            std::cerr << "write failed for " << client_label_ << ": " << std::strerror(errno)
                      << '\n';
            return false;
        }

        written += static_cast<size_t>(n);
    }

    pending_write_.clear();
    return true;
}

void Session::ProcessRequests() {
    while (true) {
        const size_t newline_pos = pending_data_.find('\n');
        if (newline_pos == std::string::npos) {
            return;
        }

        std::string request = pending_data_.substr(0, newline_pos);
        pending_data_.erase(0, newline_pos + 1);

        std::cout << "received from " << client_label_ << ": " << request << '\n';
        pending_write_ += BuildResponse(request);
    }
}

std::string Session::BuildResponse(const std::string& request) const {
    return "ok: " + request + "\n";
}
