#pragma once

#include <cstdint>
#include <csignal>
#include <unordered_map>

#include "session.h"

class Server {
public:
    explicit Server(uint16_t port);

    bool Start();
    void Run();
    void Stop();

private:
    bool InitEpoll();
    void AcceptConnections();
    void CloseConnection(int conn_fd);
    bool UpdateInterest(int conn_fd, const Session& session);

    uint16_t port_;
    int listen_fd_ = -1;
    int epoll_fd_ = -1;
    std::unordered_map<int, Session> sessions_;
};

extern volatile std::sig_atomic_t g_running;
void HandleSignal(int);
