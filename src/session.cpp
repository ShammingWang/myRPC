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

    std::string request;
    if (!ReadOnce(request)) {
        std::cout << "connection closed: " << client_label_ << '\n';
        return;
    }

    std::cout << "received from " << client_label_ << ": " << request << '\n';
    WriteAll(BuildResponse());
    std::cout << "connection closed: " << client_label_ << '\n';
}

bool Session::ReadOnce(std::string& request) {
    char buffer[kBufferSize] = {0};
    const ssize_t n = ::read(conn_fd_, buffer, sizeof(buffer) - 1);
    if (n == 0) {
        return false;
    }
    if (n < 0) {
        std::cerr << "read failed for " << client_label_ << ": " << std::strerror(errno)
                  << '\n';
        return false;
    }

    request.assign(buffer, static_cast<size_t>(n));
    return true;
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

std::string Session::BuildResponse() const {
    return "ok\n";
}
