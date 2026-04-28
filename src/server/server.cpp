#include "server/server.h"

#include "server/socket_utils.h"

#include <arpa/inet.h>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>
#include <utility>
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

Server::Server(uint16_t port, ServerOptions options)
    : port_(port),
      admin_port_(options.admin_port),
      options_(options),
      worker_pool_(std::max(1u, std::thread::hardware_concurrency())),
      observability_(std::make_shared<Observability>(ObservabilityOptions{
          options_.slow_request_threshold,
          options_.enable_request_trace,
          options_.trace_all_requests,
      })) {}

void Server::RegisterMethod(std::string service, std::string method,
                            RpcDispatcher::MethodHandler handler) {
    dispatcher_.RegisterMethod(std::move(service), std::move(method), std::move(handler));
}

void Server::RegisterService(const RpcService& service) {
    dispatcher_.RegisterService(service);
}

bool Server::Start() {
    stopping_ = false;

    listen_fd_ = CreateListenSocket(port_);
    if (listen_fd_ < 0) {
        return false;
    }

    if (!SetNonBlocking(listen_fd_)) {
        Stop();
        return false;
    }

    if (!InitAcceptorEpoll()) {
        Stop();
        return false;
    }

    if (!StartIoThreads()) {
        Stop();
        return false;
    }

    if (!StartAdminServer()) {
        Stop();
        return false;
    }

    return true;
}

void Server::Run() {
    std::cout << "server listening on 0.0.0.0:" << port_ << " with " << io_threads_.size()
              << " io threads\n";

    epoll_event events[kMaxEvents];
    while (g_running && !stopping_) {
        const int ready = ::epoll_wait(epoll_fd_, events, kMaxEvents, kEpollWaitTimeoutMs);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::cerr << "acceptor epoll_wait failed: " << std::strerror(errno) << '\n';
            break;
        }

        for (int i = 0; i < ready; ++i) {
            if (events[i].data.fd == listen_fd_) {
                AcceptConnections();
            }
        }
    }
}

void Server::Stop() {
    const bool already_stopping = stopping_.exchange(true);

    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }

    if (admin_listen_fd_ >= 0) {
        ::close(admin_listen_fd_);
        admin_listen_fd_ = -1;
    }

    for (const auto& io_thread : io_threads_) {
        WakeIoThread(*io_thread);
    }

    if (admin_thread_.joinable()) {
        admin_thread_.join();
    }

    for (const auto& io_thread : io_threads_) {
        if (io_thread->thread.joinable()) {
            io_thread->thread.join();
        }
    }

    for (const auto& io_thread : io_threads_) {
        if (io_thread->epoll_fd >= 0) {
            ::close(io_thread->epoll_fd);
            io_thread->epoll_fd = -1;
        }
        if (io_thread->wake_fd >= 0) {
            ::close(io_thread->wake_fd);
            io_thread->wake_fd = -1;
        }
    }
    io_threads_.clear();

    worker_pool_.Stop();

    if (epoll_fd_ >= 0) {
        ::close(epoll_fd_);
        epoll_fd_ = -1;
    }

    if (already_stopping) {
        return;
    }
}

bool Server::InitAcceptorEpoll() {
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

    return true;
}

bool Server::StartIoThreads() {
    const size_t io_thread_count =
        options_.io_thread_count == 0
            ? static_cast<size_t>(std::max(1u, std::thread::hardware_concurrency()))
            : options_.io_thread_count;

    io_threads_.clear();
    io_threads_.reserve(io_thread_count);

    for (size_t i = 0; i < io_thread_count; ++i) {
        auto io_thread = std::make_unique<IoThreadState>();
        io_thread->index = i;
        if (!InitIoThread(*io_thread)) {
            return false;
        }
        io_threads_.push_back(std::move(io_thread));
    }

    for (size_t i = 0; i < io_threads_.size(); ++i) {
        observability_->RegisterIoThread(i);
        io_threads_[i]->thread = std::thread(&Server::RunIoThread, this, i);
    }

    return true;
}

bool Server::InitIoThread(IoThreadState& io_thread) {
    io_thread.epoll_fd = ::epoll_create1(0);
    if (io_thread.epoll_fd < 0) {
        std::cerr << "epoll_create1 failed for io thread " << io_thread.index << ": "
                  << std::strerror(errno) << '\n';
        return false;
    }

    io_thread.wake_fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (io_thread.wake_fd < 0) {
        std::cerr << "eventfd failed for io thread " << io_thread.index << ": "
                  << std::strerror(errno) << '\n';
        return false;
    }

    epoll_event event{};
    event.events = EPOLLIN;
    event.data.fd = io_thread.wake_fd;
    if (::epoll_ctl(io_thread.epoll_fd, EPOLL_CTL_ADD, io_thread.wake_fd, &event) < 0) {
        std::cerr << "epoll_ctl add wake fd failed for io thread " << io_thread.index << ": "
                  << std::strerror(errno) << '\n';
        return false;
    }

    return true;
}

bool Server::StartAdminServer() {
    if (admin_port_ == 0) {
        return true;
    }

    admin_listen_fd_ = CreateListenSocket(admin_port_);
    if (admin_listen_fd_ < 0) {
        return false;
    }
    if (!SetNonBlocking(admin_listen_fd_)) {
        return false;
    }

    admin_thread_ = std::thread(&Server::RunAdminThread, this);
    return true;
}

void Server::RunIoThread(size_t io_thread_index) {
    IoThreadState& io_thread = *io_threads_[io_thread_index];
    epoll_event events[kMaxEvents];

    while (!stopping_) {
        const int ready =
            ::epoll_wait(io_thread.epoll_fd, events, kMaxEvents, kEpollWaitTimeoutMs);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (!stopping_) {
                std::cerr << "io thread " << io_thread.index
                          << " epoll_wait failed: " << std::strerror(errno) << '\n';
            }
            break;
        }

        const auto processing_started_at = std::chrono::steady_clock::now();
        if (ready == 0) {
            observability_->OnIoThreadLoopIteration(io_thread.index, 0, std::chrono::microseconds{0});
            CloseIdleConnections(io_thread);
            continue;
        }

        for (int i = 0; i < ready; ++i) {
            const int fd = events[i].data.fd;
            const uint32_t triggered = events[i].events;

            if (fd == io_thread.wake_fd) {
                if (!DrainIoThreadQueues(io_thread)) {
                    return;
                }
                continue;
            }

            auto it = io_thread.connections.find(fd);
            if (it == io_thread.connections.end()) {
                continue;
            }

            bool keep_open = true;
            if ((triggered & (EPOLLERR | EPOLLHUP)) != 0) {
                keep_open = false;
            }
            if (keep_open && (triggered & (EPOLLIN | EPOLLRDHUP)) != 0) {
                keep_open = it->second.connection.OnReadable();
            }
            if (keep_open && (triggered & EPOLLOUT) != 0) {
                keep_open = it->second.connection.OnWritable();
            }
            if (keep_open && it->second.connection.ShouldClose()) {
                keep_open = false;
            }

            if (!keep_open) {
                CloseConnection(io_thread, fd);
                continue;
            }

            if (!UpdateInterest(io_thread, fd, it->second.connection)) {
                CloseConnection(io_thread, fd);
            }
        }

        const auto processing_finished_at = std::chrono::steady_clock::now();
        observability_->OnIoThreadLoopIteration(
            io_thread.index, static_cast<size_t>(ready),
            std::chrono::duration_cast<std::chrono::microseconds>(processing_finished_at -
                                                                  processing_started_at));
    }

    for (auto it = io_thread.connections.begin(); it != io_thread.connections.end();) {
        const int conn_fd = it->first;
        ++it;
        CloseConnection(io_thread, conn_fd);
    }
}

void Server::RunAdminThread() {
    while (!stopping_) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        const int client_fd =
            ::accept(admin_listen_fd_, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            if (stopping_) {
                return;
            }
            std::cerr << "admin accept failed: " << std::strerror(errno) << '\n';
            continue;
        }

        HandleAdminClient(client_fd);
        ::close(client_fd);
    }
}

void Server::HandleAdminClient(int client_fd) {
    std::string request_buffer;
    request_buffer.reserve(1024);

    char buffer[1024];
    while (request_buffer.find("\r\n\r\n") == std::string::npos) {
        const ssize_t n = ::recv(client_fd, buffer, sizeof(buffer), 0);
        if (n == 0) {
            return;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }
        request_buffer.append(buffer, static_cast<size_t>(n));
        if (request_buffer.size() > 8192) {
            break;
        }
    }

    const bool is_metrics_request =
        request_buffer.rfind("GET /metrics ", 0) == 0 ||
        request_buffer.rfind("GET /metrics?", 0) == 0;
    const std::string body =
        is_metrics_request ? observability_->ExportMetrics() : "not found\n";
    const std::string status_line =
        is_metrics_request ? "HTTP/1.1 200 OK\r\n" : "HTTP/1.1 404 Not Found\r\n";
    const std::string headers =
        "Content-Type: text/plain; version=0.0.4\r\nContent-Length: " +
        std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
    const std::string response = status_line + headers + body;

    size_t sent = 0;
    while (sent < response.size()) {
        const ssize_t n = ::send(client_fd, response.data() + sent, response.size() - sent, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }
        sent += static_cast<size_t>(n);
    }
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
        if (::inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip)) ==
            nullptr) {
            std::cerr << "inet_ntop failed: " << std::strerror(errno) << '\n';
            ::close(conn_fd);
            continue;
        }

        const size_t io_thread_index = next_io_thread_.fetch_add(1) % io_threads_.size();
        AcceptedConnection connection;
        connection.conn_fd = conn_fd;
        connection.connection_id = next_connection_id_.fetch_add(1);
        connection.client_label =
            std::string(client_ip) + ":" + std::to_string(::ntohs(client_addr.sin_port));
        EnqueueAcceptedConnection(io_thread_index, std::move(connection));
    }
}

void Server::EnqueueAcceptedConnection(size_t io_thread_index, AcceptedConnection connection) {
    if (io_thread_index >= io_threads_.size()) {
        ::close(connection.conn_fd);
        return;
    }

    IoThreadState& io_thread = *io_threads_[io_thread_index];
    {
        std::lock_guard<std::mutex> lock(io_thread.mutex);
        io_thread.accepted_connections.push(std::move(connection));
        observability_->SetIoThreadPendingResponseCount(io_thread.index,
                                                        io_thread.completed_responses.size());
    }
    WakeIoThread(io_thread);
}

void Server::CloseConnection(IoThreadState& io_thread, int conn_fd) {
    auto it = io_thread.connections.find(conn_fd);
    if (it == io_thread.connections.end()) {
        return;
    }

    it->second.connection.OnClosed();
    if (::epoll_ctl(io_thread.epoll_fd, EPOLL_CTL_DEL, conn_fd, nullptr) < 0 && errno != ENOENT &&
        errno != EBADF) {
        std::cerr << "epoll_ctl del conn fd failed: " << std::strerror(errno) << '\n';
    }
    ::close(conn_fd);
    io_thread.connections.erase(it);
    observability_->SetIoThreadConnectionCount(io_thread.index, io_thread.connections.size());
}

bool Server::UpdateInterest(IoThreadState& io_thread, int conn_fd, const Connection& connection) {
    epoll_event event{};
    event.events = BuildConnectionEvents(connection);
    event.data.fd = conn_fd;
    if (::epoll_ctl(io_thread.epoll_fd, EPOLL_CTL_MOD, conn_fd, &event) < 0) {
        std::cerr << "epoll_ctl mod conn fd failed: " << std::strerror(errno) << '\n';
        return false;
    }
    return true;
}

void Server::DispatchRequest(size_t io_thread_index, int conn_fd, uint64_t connection_id,
                             RpcRequest request) {
    if (stopping_) {
        RpcResponse response;
        response.request_id = request.request_id;
        response.serialization = request.serialization;
        response.status_code = RpcStatusCode::kServerOverloaded;
        response.payload = "server is stopping";
        EnqueueResponse(io_thread_index, conn_fd, connection_id, std::move(request),
                        std::move(response), std::chrono::steady_clock::now());
        return;
    }

    const uint64_t request_id = request.request_id;
    const RpcSerializationType serialization = request.serialization;
    request.enqueued_at = std::chrono::steady_clock::now();
    observability_->OnRequestDispatched();
    const bool submitted =
        worker_pool_.Submit([this, io_thread_index, conn_fd, connection_id,
                             request = std::move(request)]() mutable {
            request.worker_started_at = std::chrono::steady_clock::now();
            if (IsRequestTimedOut(request, std::chrono::steady_clock::now())) {
                EnqueueResponse(io_thread_index, conn_fd, connection_id,
                                std::move(request), BuildTimeoutResponse(request),
                                std::chrono::steady_clock::now());
                return;
            }

            RpcResponse response = dispatcher_.Dispatch(request);
            if (IsSuccess(response.status_code) &&
                IsRequestTimedOut(request, std::chrono::steady_clock::now())) {
                response = BuildTimeoutResponse(request);
            }
            EnqueueResponse(io_thread_index, conn_fd, connection_id, std::move(request),
                            std::move(response), std::chrono::steady_clock::now());
        });

    if (!submitted) {
        observability_->OnWorkerPoolRejected();
        RpcResponse response;
        response.request_id = request_id;
        response.serialization = serialization;
        response.status_code = RpcStatusCode::kServerOverloaded;
        response.payload = "worker pool unavailable";
        EnqueueResponse(io_thread_index, conn_fd, connection_id, std::move(request),
                        std::move(response), std::chrono::steady_clock::now());
    }
}

void Server::EnqueueResponse(size_t io_thread_index, int conn_fd, uint64_t connection_id,
                             RpcRequest request, RpcResponse response,
                             std::chrono::steady_clock::time_point worker_finished_at) {
    if (stopping_ || io_thread_index >= io_threads_.size()) {
        return;
    }

    IoThreadState& io_thread = *io_threads_[io_thread_index];
    {
        std::lock_guard<std::mutex> lock(io_thread.mutex);
        io_thread.completed_responses.push(PendingResponse{
            io_thread_index, conn_fd, connection_id, std::move(request), std::move(response),
            worker_finished_at, std::chrono::steady_clock::now()});
        observability_->SetIoThreadPendingResponseCount(io_thread.index,
                                                        io_thread.completed_responses.size());
    }
    WakeIoThread(io_thread);
}

bool Server::DrainIoThreadQueues(IoThreadState& io_thread) {
    while (true) {
        uint64_t wake_value = 0;
        const ssize_t n = ::read(io_thread.wake_fd, &wake_value, sizeof(wake_value));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            std::cerr << "wake read failed for io thread " << io_thread.index << ": "
                      << std::strerror(errno) << '\n';
            return false;
        }
        break;
    }

    std::queue<AcceptedConnection> accepted_connections;
    std::queue<PendingResponse> completed_responses;
    {
        std::lock_guard<std::mutex> lock(io_thread.mutex);
        accepted_connections.swap(io_thread.accepted_connections);
        completed_responses.swap(io_thread.completed_responses);
        observability_->SetIoThreadPendingResponseCount(io_thread.index,
                                                        io_thread.completed_responses.size());
    }

    DrainAcceptedConnections(io_thread, accepted_connections);
    DrainCompletedResponses(io_thread, completed_responses);
    return true;
}

void Server::DrainAcceptedConnections(IoThreadState& io_thread,
                                      std::queue<AcceptedConnection>& accepted_connections) {
    while (!accepted_connections.empty()) {
        AcceptedConnection accepted = std::move(accepted_connections.front());
        accepted_connections.pop();

        Connection connection(
            accepted.conn_fd, accepted.client_label,
            [this, io_thread_index = io_thread.index, conn_fd = accepted.conn_fd,
             connection_id = accepted.connection_id](RpcRequest request) {
                DispatchRequest(io_thread_index, conn_fd, connection_id, std::move(request));
            },
            ConnectionOptions{options_.max_pending_requests_per_connection,
                              options_.max_outbound_buffer_bytes_per_connection,
                              io_thread.index,
                              accepted.connection_id,
                              observability_});

        epoll_event event{};
        event.events = BuildConnectionEvents(connection);
        event.data.fd = accepted.conn_fd;
        if (::epoll_ctl(io_thread.epoll_fd, EPOLL_CTL_ADD, accepted.conn_fd, &event) < 0) {
            std::cerr << "epoll_ctl add conn fd failed: " << std::strerror(errno) << '\n';
            ::close(accepted.conn_fd);
            continue;
        }

        connection.OnConnected();
        io_thread.connections.emplace(
            accepted.conn_fd,
            ConnectionEntry{accepted.connection_id, std::move(connection)});
        observability_->SetIoThreadConnectionCount(io_thread.index, io_thread.connections.size());
    }
}

void Server::DrainCompletedResponses(IoThreadState& io_thread,
                                     std::queue<PendingResponse>& completed_responses) {
    while (!completed_responses.empty()) {
        PendingResponse pending = std::move(completed_responses.front());
        completed_responses.pop();

        auto it = io_thread.connections.find(pending.conn_fd);
        if (it == io_thread.connections.end()) {
            continue;
        }
        if (it->second.connection_id != pending.connection_id) {
            continue;
        }

        if (!it->second.connection.QueueResponse(Connection::ResponseContext{
                std::move(pending.request),
                std::move(pending.response),
                pending.worker_finished_at,
                pending.response_enqueued_at,
                std::chrono::steady_clock::now(),
            })) {
            CloseConnection(io_thread, pending.conn_fd);
            continue;
        }
        if (it->second.connection.ShouldClose()) {
            CloseConnection(io_thread, pending.conn_fd);
            continue;
        }
        if (!UpdateInterest(io_thread, pending.conn_fd, it->second.connection)) {
            CloseConnection(io_thread, pending.conn_fd);
        }
    }
}

void Server::CloseIdleConnections(IoThreadState& io_thread) {
    const auto now = std::chrono::steady_clock::now();
    for (auto it = io_thread.connections.begin(); it != io_thread.connections.end();) {
        if (!it->second.connection.IsIdleFor(now, options_.idle_connection_timeout)) {
            ++it;
            continue;
        }

        const int conn_fd = it->first;
        ++it;
        CloseConnection(io_thread, conn_fd);
    }
}

void Server::WakeIoThread(IoThreadState& io_thread) const {
    if (io_thread.wake_fd < 0) {
        return;
    }

    observability_->OnIoThreadWakeup(io_thread.index);
    const uint64_t wake_value = 1;
    if (::write(io_thread.wake_fd, &wake_value, sizeof(wake_value)) < 0 && errno != EAGAIN) {
        std::cerr << "wake write failed for io thread " << io_thread.index << ": "
                  << std::strerror(errno) << '\n';
    }
}

RpcResponse Server::BuildTimeoutResponse(const RpcRequest& request) const {
    RpcResponse response;
    response.request_id = request.request_id;
    response.serialization = request.serialization;
    response.status_code = RpcStatusCode::kRequestTimeout;
    response.payload = "request timed out before completion";
    return response;
}

void HandleSignal(int) {
    g_running = 0;
}
