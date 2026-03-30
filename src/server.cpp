#include "server.h"

#include "session.h"
#include "socket_utils.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <unistd.h>

#include <netinet/in.h>
#include <sys/socket.h>

volatile std::sig_atomic_t g_running = 1;

Server::Server(uint16_t port) : port_(port) {}

bool Server::Start() {
    listen_fd_ = CreateListenSocket(port_);
    return listen_fd_ >= 0;
}

void Server::Run() {
    std::cout << "server listening on 0.0.0.0:" << port_ << '\n';

    while (g_running) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        const int conn_fd =
            ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&client_addr), &client_len);

        if (conn_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::cerr << "accept failed: " << std::strerror(errno) << '\n';
            continue;
        }

        HandleConnection(conn_fd);
    }
}

void Server::Stop() {
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
}

void Server::HandleConnection(int conn_fd) const {
    char client_ip[INET_ADDRSTRLEN] = {0};
    sockaddr_in client_addr{};
    socklen_t client_len = sizeof(client_addr);
    if (::getpeername(conn_fd, reinterpret_cast<sockaddr*>(&client_addr), &client_len) < 0) {
        std::cerr << "getpeername failed: " << std::strerror(errno) << '\n';
        ::close(conn_fd);
        return;
    }

    ::inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
    Session session(conn_fd,
                    std::string(client_ip) + ":" + std::to_string(::ntohs(client_addr.sin_port)));
    session.Run();
    ::close(conn_fd);
}

void HandleSignal(int) {
    g_running = 0;
}
