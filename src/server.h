#pragma once

#include <cstdint>
#include <csignal>

class Server {
public:
    explicit Server(uint16_t port);

    bool Start();
    void Run();
    void Stop();

private:
    void HandleConnection(int conn_fd) const;

    uint16_t port_;
    int listen_fd_ = -1;
};

extern volatile std::sig_atomic_t g_running;
void HandleSignal(int);
