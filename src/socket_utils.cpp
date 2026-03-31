#include "socket_utils.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <unistd.h>

#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>

SocketHandle::~SocketHandle() {
    reset();
}

SocketHandle::SocketHandle(SocketHandle&& other) noexcept : fd_(other.release()) {}

SocketHandle& SocketHandle::operator=(SocketHandle&& other) noexcept {
    if (this != &other) {
        reset(other.release());
    }
    return *this;
}

int SocketHandle::release() {
    const int fd = fd_;
    fd_ = -1;
    return fd;
}

void SocketHandle::reset(int fd) {
    if (fd_ >= 0) {
        ::close(fd_);
    }
    fd_ = fd;
}

int CreateListenSocket(uint16_t port) {
    const int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        std::cerr << "socket failed: " << std::strerror(errno) << '\n';
        return -1;
    }

    SocketHandle guard(listen_fd);
    const int opt = 1;
    if (::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        std::cerr << "setsockopt failed: " << std::strerror(errno) << '\n';
        return -1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = ::htonl(INADDR_ANY);  // INADDR_LOOPBACK
    addr.sin_port = ::htons(port);
    
    if (::bind(listen_fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "bind failed: " << std::strerror(errno) << '\n';
        return -1;
    }

    constexpr int kBacklog = 4096;
    if (::listen(listen_fd, kBacklog) < 0) {
        std::cerr << "listen failed: " << std::strerror(errno) << '\n';
        return -1;
    }

    return guard.release();
}

bool SetNonBlocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        std::cerr << "fcntl(F_GETFL) failed: " << std::strerror(errno) << '\n';
        return false;
    }

    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        std::cerr << "fcntl(F_SETFL) failed: " << std::strerror(errno) << '\n';
        return false;
    }

    return true;
}
