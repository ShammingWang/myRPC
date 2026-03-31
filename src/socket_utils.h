#pragma once

#include <cstdint>

class SocketHandle {
public:
    SocketHandle() = default;
    explicit SocketHandle(int fd) : fd_(fd) {}
    ~SocketHandle();

    SocketHandle(const SocketHandle&) = delete;
    SocketHandle& operator=(const SocketHandle&) = delete;

    SocketHandle(SocketHandle&& other) noexcept;
    SocketHandle& operator=(SocketHandle&& other) noexcept;

    int get() const { return fd_; }
    bool valid() const { return fd_ >= 0; }
    int release();
    void reset(int fd = -1);

private:
    int fd_ = -1;
};

int CreateListenSocket(uint16_t port);
bool SetNonBlocking(int fd);
