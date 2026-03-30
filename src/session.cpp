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

void Session::Run() {
    std::cout << "accepted connection from " << client_label_ << '\n';

    while (true) {
        std::string request;
        if (!ReadLine(request)) {
            break;
        }

        std::cout << "received from " << client_label_ << ": " << request << '\n';
        if (!WriteAll(BuildResponse(request))) {
            break;
        }
    }

    std::cout << "connection closed: " << client_label_ << '\n';
}

bool Session::ReadLine(std::string& line) {
    while (true) {
        const size_t newline_pos = pending_data_.find('\n');
        if (newline_pos != std::string::npos) {
            line = pending_data_.substr(0, newline_pos);
            pending_data_.erase(0, newline_pos + 1);
            return true;
        }

        char buffer[kBufferSize];
        const ssize_t n = ::read(conn_fd_, buffer, sizeof(buffer));
        if (n == 0) {
            return false;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::cerr << "read failed for " << client_label_ << ": " << std::strerror(errno)
                      << '\n';
            return false;
        }

        pending_data_.append(buffer, static_cast<size_t>(n));
    }
}

bool Session::WriteAll(const std::string& response) {
    size_t written = 0;
    while (written < response.size()) {
        const ssize_t n =
            ::write(conn_fd_, response.data() + written, response.size() - written);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::cerr << "write failed for " << client_label_ << ": " << std::strerror(errno)
                      << '\n';
            return false;
        }

        written += static_cast<size_t>(n);
    }

    return true;
}

std::string Session::BuildResponse(const std::string& request) const {
    return "ok: " + request + "\n";
}
