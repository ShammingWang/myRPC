#pragma once

#include <csignal>
#include <cstdint>
#include <string>
#include <unordered_map>

#include "connection.h"
#include "rpc_dispatcher.h"

class Server {
public:
    explicit Server(uint16_t port);

    void RegisterHandler(std::string method, RpcDispatcher::Handler handler);
    bool Start();
    void Run();
    void Stop();

private:
    bool InitEpoll();
    void AcceptConnections();
    void CloseConnection(int conn_fd);
    bool UpdateInterest(int conn_fd, const Connection& connection);

    uint16_t port_;
    int listen_fd_ = -1;
    int epoll_fd_ = -1;
    RpcDispatcher dispatcher_;
    std::unordered_map<int, Connection> connections_;
};

extern volatile std::sig_atomic_t g_running;
void HandleSignal(int);
