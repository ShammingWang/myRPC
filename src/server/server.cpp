#include "server/server.h"

#include "server/socket_utils.h"

#include <arpa/inet.h>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>
#include <unistd.h>

#include <netinet/in.h>
#include <sys/eventfd.h>
#include <sys/epoll.h>
#include <sys/socket.h>

namespace {

constexpr int kMaxEvents = 64;
constexpr int kEpollWaitTimeoutMs = 1000;

uint32_t BuildConnectionEvents(const Connection& connection) {
    uint32_t events = EPOLLRDHUP;
    if (!connection.ShouldClose()) {
        events |= EPOLLIN;
    }
    if (connection.WantsWrite()) {
        events |= EPOLLOUT;
    }
    return events;
}

}  // namespace

volatile std::sig_atomic_t g_running = 1;

Server::Server(uint16_t port)
    : port_(port), worker_pool_(std::max(1u, std::thread::hardware_concurrency())) {}

void Server::RegisterHandler(std::string method, RpcDispatcher::Handler handler) {
    dispatcher_.RegisterHandler(std::move(method), std::move(handler));
}

bool Server::Start() {
    listen_fd_ = CreateListenSocket(port_);
    if (listen_fd_ < 0) {
        return false;
    }

    if (!SetNonBlocking(listen_fd_)) {
        Stop();
        return false;
    }

    if (!InitEpoll()) {
        Stop();
        return false;
    }

    return true;
}

void Server::Run() {
    std::cout << "server listening on 0.0.0.0:" << port_ << '\n';

    epoll_event events[kMaxEvents];
    while (g_running) {
        const int ready = ::epoll_wait(epoll_fd_, events, kMaxEvents, kEpollWaitTimeoutMs);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::cerr << "epoll_wait failed: " << std::strerror(errno) << '\n';
            break;
        }

        for (int i = 0; i < ready; ++i) {
            const int fd = events[i].data.fd;
            const uint32_t triggered = events[i].events;

            if (fd == listen_fd_) {
                AcceptConnections();
                continue;
            }
            if (fd == wake_fd_) {
                if (!DrainCompletedResponses()) {
                    break;
                }
                continue;
            }

            auto it = connections_.find(fd);
            if (it == connections_.end()) {
                continue;
            }

            bool keep_open = true;
            if ((triggered & (EPOLLERR | EPOLLHUP)) != 0) {
                keep_open = false;
            }
            if (keep_open && (triggered & (EPOLLIN | EPOLLRDHUP)) != 0) {
                keep_open = it->second.OnReadable();
            }
            if (keep_open && (triggered & EPOLLOUT) != 0) {
                keep_open = it->second.OnWritable();
            }
            if (keep_open && it->second.ShouldClose()) {
                keep_open = false;
            }

            if (!keep_open) {
                CloseConnection(fd);
                continue;
            }

            if (!UpdateInterest(fd, it->second)) {
                CloseConnection(fd);
            }
        }
    }
}

void Server::Stop() {
    worker_pool_.Stop();

    for (auto it = connections_.begin(); it != connections_.end();) {
        const int conn_fd = it->first;
        ++it;
        CloseConnection(conn_fd);
    }

    if (epoll_fd_ >= 0) {
        ::close(epoll_fd_);
        epoll_fd_ = -1;
    }

    if (wake_fd_ >= 0) {
        ::close(wake_fd_);
        wake_fd_ = -1;
    }

    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
}

bool Server::InitEpoll() {
    epoll_fd_ = ::epoll_create1(0);
    if (epoll_fd_ < 0) {
        std::cerr << "epoll_create1 failed: " << std::strerror(errno) << '\n';
        return false;
    }

    epoll_event event{};
    event.events = EPOLLIN;
    event.data.fd = listen_fd_;
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, listen_fd_, &event) < 0) {
        std::cerr << "epoll_ctl add listen fd failed: " << std::strerror(errno) << '\n';
        return false;
    }

    wake_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wake_fd_ < 0) {
        std::cerr << "eventfd failed: " << std::strerror(errno) << '\n';
        return false;
    }

    event = {};
    event.events = EPOLLIN;
    event.data.fd = wake_fd_;
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, wake_fd_, &event) < 0) {
        std::cerr << "epoll_ctl add wake fd failed: " << std::strerror(errno) << '\n';
        return false;
    }

    return true;
}

void Server::AcceptConnections() {
    while (true) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        const int conn_fd =
            ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&client_addr), &client_len);

        if (conn_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            std::cerr << "accept failed: " << std::strerror(errno) << '\n';
            return;
        }

        if (!SetNonBlocking(conn_fd)) {
            ::close(conn_fd);
            continue;
        }

        char client_ip[INET_ADDRSTRLEN] = {0};
        if (::inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip)) == nullptr) {
            std::cerr << "inet_ntop failed: " << std::strerror(errno) << '\n';
            ::close(conn_fd);
            continue;
        }

        Connection connection(
            conn_fd, std::string(client_ip) + ":" + std::to_string(::ntohs(client_addr.sin_port)),
            [this, conn_fd](RpcRequest request) { DispatchRequest(conn_fd, std::move(request)); });
        connection.OnConnected();

        epoll_event event{};
        event.events = BuildConnectionEvents(connection);
        event.data.fd = conn_fd;
        if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, conn_fd, &event) < 0) {
            std::cerr << "epoll_ctl add conn fd failed: " << std::strerror(errno) << '\n';
            ::close(conn_fd);
            continue;
        }

        connections_.emplace(conn_fd, std::move(connection));
    }
}

void Server::CloseConnection(int conn_fd) {
    auto it = connections_.find(conn_fd);
    if (it == connections_.end()) {
        return;
    }

    it->second.OnClosed();
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, conn_fd, nullptr) < 0 && errno != ENOENT &&
        errno != EBADF) {
        std::cerr << "epoll_ctl del conn fd failed: " << std::strerror(errno) << '\n';
    }
    ::close(conn_fd);
    connections_.erase(it);
}

bool Server::UpdateInterest(int conn_fd, const Connection& connection) {
    epoll_event event{};
    event.events = BuildConnectionEvents(connection);
    event.data.fd = conn_fd;
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, conn_fd, &event) < 0) {
        std::cerr << "epoll_ctl mod conn fd failed: " << std::strerror(errno) << '\n';
        return false;
    }
    return true;
}

void Server::DispatchRequest(int conn_fd, RpcRequest request) {
    const uint64_t request_id = request.request_id;
    const bool submitted = worker_pool_.Submit([this, conn_fd, request = std::move(request)]() mutable {
        RpcResponse response = dispatcher_.Dispatch(request);
        EnqueueResponse(conn_fd, std::move(response));
    });

    if (!submitted) {
        RpcResponse response;
        response.request_id = request_id;
        response.status_code = 503;
        response.payload = "worker pool unavailable";
        EnqueueResponse(conn_fd, std::move(response));
    }
}

void Server::EnqueueResponse(int conn_fd, RpcResponse response) {
    {
        std::lock_guard<std::mutex> lock(completed_mutex_);
        completed_responses_.push(PendingResponse{conn_fd, std::move(response)});
    }

    const uint64_t wake_value = 1;
    if (::write(wake_fd_, &wake_value, sizeof(wake_value)) < 0 && errno != EAGAIN) {
        std::cerr << "wake write failed: " << std::strerror(errno) << '\n';
    }
}

bool Server::DrainCompletedResponses() {
    while (true) {
        uint64_t wake_value = 0;
        const ssize_t n = ::read(wake_fd_, &wake_value, sizeof(wake_value));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            std::cerr << "wake read failed: " << std::strerror(errno) << '\n';
            return false;
        }
        break;
    }

    std::queue<PendingResponse> completed;
    {
        std::lock_guard<std::mutex> lock(completed_mutex_);
        completed.swap(completed_responses_);
    }

    while (!completed.empty()) {
        PendingResponse pending = std::move(completed.front());
        completed.pop();

        auto it = connections_.find(pending.conn_fd);
        if (it == connections_.end()) {
            continue;
        }

        it->second.QueueResponse(pending.response);
        if (it->second.ShouldClose()) {
            CloseConnection(pending.conn_fd);
            continue;
        }
        if (!UpdateInterest(pending.conn_fd, it->second)) {
            CloseConnection(pending.conn_fd);
        }
    }

    return true;
}

void HandleSignal(int) {
    g_running = 0;
}
