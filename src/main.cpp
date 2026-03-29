#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <iostream>
#include <string>
#include <unistd.h>

#include <netinet/in.h>
#include <sys/socket.h>

namespace {

constexpr int kBacklog = 128;
constexpr uint16_t kDefaultPort = 8080;
constexpr size_t kBufferSize = 4096;
volatile std::sig_atomic_t g_running = 1;

void HandleSignal(int) {
    g_running = 0;
}

int CreateListenSocket(uint16_t port) {
    const int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        std::cerr << "socket failed: " << std::strerror(errno) << '\n';
        return -1;
    }

    const int opt = 1;
    if (::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        std::cerr << "setsockopt failed: " << std::strerror(errno) << '\n';
        ::close(listen_fd);
        return -1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    addr.sin_port = ::htons(port);

    if (::bind(listen_fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "bind failed: " << std::strerror(errno) << '\n';
        ::close(listen_fd);
        return -1;
    }

    if (::listen(listen_fd, kBacklog) < 0) {
        std::cerr << "listen failed: " << std::strerror(errno) << '\n';
        ::close(listen_fd);
        return -1;
    }

    return listen_fd;
}

void ServeForever(int listen_fd) {
    std::cout << "server listening on 127.0.0.1:" << kDefaultPort << '\n';

    while (g_running) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        const int conn_fd =
            ::accept(listen_fd, reinterpret_cast<sockaddr*>(&client_addr), &client_len);

        if (conn_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::cerr << "accept failed: " << std::strerror(errno) << '\n';
            continue;
        }

        char client_ip[INET_ADDRSTRLEN] = {0};
        ::inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
        std::cout << "accepted connection from " << client_ip << ':'
                  << ::ntohs(client_addr.sin_port) << '\n';

        char buffer[kBufferSize] = {0};
        const ssize_t n = ::read(conn_fd, buffer, sizeof(buffer) - 1);
        if (n < 0) {
            std::cerr << "read failed: " << std::strerror(errno) << '\n';
            ::close(conn_fd);
            continue;
        }

        const std::string request(buffer, static_cast<size_t>(n));
        std::cout << "received " << n << " bytes: " << request << '\n';

        const std::string response = "ok\n";
        if (::write(conn_fd, response.data(), response.size()) < 0) {
            std::cerr << "write failed: " << std::strerror(errno) << '\n';
        }

        ::close(conn_fd);
    }
}

}  // namespace

int main() {
    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);

    const int listen_fd = CreateListenSocket(kDefaultPort);
    if (listen_fd < 0) {
        return 1;
    }

    ServeForever(listen_fd);
    ::close(listen_fd);
    std::cout << "server stopped\n";
    return 0;
}
