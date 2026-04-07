#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr uint32_t kMagic = 0x4D525043;
constexpr uint8_t kVersion = 1;
constexpr uint8_t kRequestType = 1;
constexpr uint8_t kResponseType = 2;
constexpr size_t kHeaderSize = 24;

struct Args {
    std::string host = "127.0.0.1";
    uint16_t port = 8080;
    std::string method = "echo";
    std::string payload = "hello rpc";
    int connections = 100;
    uint64_t requests = 10000;
    double duration = 0.0;
    double timeout = 5.0;
    uint16_t expect_status = 0;
    std::optional<std::string> expect_payload;
    bool reconnect_per_request = false;
};

struct Response {
    uint64_t request_id = 0;
    uint16_t status = 0;
    std::string body;
};

struct SharedState {
    explicit SharedState(const Args& args)
        : request_limit(args.requests),
          duration_seconds(args.duration),
          start(std::chrono::steady_clock::now()) {}

    std::optional<uint64_t> AcquireRequestIndex() {
        while (true) {
            if (stop.load(std::memory_order_relaxed)) {
                return std::nullopt;
            }
            if (duration_seconds > 0.0) {
                const auto elapsed = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - start);
                if (elapsed.count() >= duration_seconds) {
                    stop.store(true, std::memory_order_relaxed);
                    return std::nullopt;
                }
            }

            const uint64_t current = next_request.fetch_add(1, std::memory_order_relaxed);
            if (duration_seconds > 0.0) {
                return current;
            }
            if (current >= request_limit) {
                return std::nullopt;
            }
            return current;
        }
    }

    void RecordSuccess(double latency_ms, size_t sent_bytes, size_t recv_bytes) {
        success.fetch_add(1, std::memory_order_relaxed);
        bytes_sent.fetch_add(sent_bytes, std::memory_order_relaxed);
        bytes_received.fetch_add(recv_bytes, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(latencies_mutex);
            latencies_ms.push_back(latency_ms);
        }
        if (duration_seconds <= 0.0 &&
            success.load(std::memory_order_relaxed) >= request_limit) {
            stop.store(true, std::memory_order_relaxed);
        }
    }

    void RecordFailure(const std::string& message) {
        failures.fetch_add(1, std::memory_order_relaxed);
        stop.store(true, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(error_mutex);
        if (first_error.empty()) {
            first_error = message;
        }
    }

    uint64_t request_limit;
    double duration_seconds;
    std::chrono::steady_clock::time_point start;
    std::atomic<uint64_t> next_request{0};
    std::atomic<uint64_t> success{0};
    std::atomic<uint64_t> failures{0};
    std::atomic<uint64_t> bytes_sent{0};
    std::atomic<uint64_t> bytes_received{0};
    std::atomic<bool> stop{false};
    std::mutex latencies_mutex;
    std::vector<double> latencies_ms;
    std::mutex error_mutex;
    std::string first_error;
};

struct SocketHandle {
    SocketHandle() = default;
    explicit SocketHandle(int fd) : fd(fd) {}
    ~SocketHandle() {
        if (fd >= 0) {
            ::close(fd);
        }
    }

    SocketHandle(const SocketHandle&) = delete;
    SocketHandle& operator=(const SocketHandle&) = delete;

    SocketHandle(SocketHandle&& other) noexcept : fd(other.fd) {
        other.fd = -1;
    }

    SocketHandle& operator=(SocketHandle&& other) noexcept {
        if (this != &other) {
            if (fd >= 0) {
                ::close(fd);
            }
            fd = other.fd;
            other.fd = -1;
        }
        return *this;
    }

    int release() {
        const int value = fd;
        fd = -1;
        return value;
    }

    int fd = -1;
};

void AppendUint16(std::string& out, uint16_t value) {
    out.push_back(static_cast<char>((value >> 8) & 0xff));
    out.push_back(static_cast<char>(value & 0xff));
}

void AppendUint32(std::string& out, uint32_t value) {
    out.push_back(static_cast<char>((value >> 24) & 0xff));
    out.push_back(static_cast<char>((value >> 16) & 0xff));
    out.push_back(static_cast<char>((value >> 8) & 0xff));
    out.push_back(static_cast<char>(value & 0xff));
}

void AppendUint64(std::string& out, uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<char>((value >> shift) & 0xff));
    }
}

uint16_t LoadUint16(const char* data) {
    return (static_cast<uint16_t>(static_cast<unsigned char>(data[0])) << 8) |
           static_cast<uint16_t>(static_cast<unsigned char>(data[1]));
}

uint32_t LoadUint32(const char* data) {
    return (static_cast<uint32_t>(static_cast<unsigned char>(data[0])) << 24) |
           (static_cast<uint32_t>(static_cast<unsigned char>(data[1])) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(data[2])) << 8) |
           static_cast<uint32_t>(static_cast<unsigned char>(data[3]));
}

uint64_t LoadUint64(const char* data) {
    uint64_t value = 0;
    for (size_t i = 0; i < sizeof(uint64_t); ++i) {
        value = (value << 8) | static_cast<unsigned char>(data[i]);
    }
    return value;
}

void PrintUsage(const char* program) {
    std::cout
        << "Usage: " << program << " [options]\n"
        << "  --host <host>                  Server host (default: 127.0.0.1)\n"
        << "  --port <port>                  Server port (default: 8080)\n"
        << "  --method <method>              RPC method name (default: echo)\n"
        << "  --payload <payload>            RPC payload (default: hello rpc)\n"
        << "  --connections <n>              Concurrent connections (default: 100)\n"
        << "  --requests <n>                 Total requests in count mode (default: 10000)\n"
        << "  --duration <seconds>           Run benchmark for a fixed duration\n"
        << "  --timeout <seconds>            Socket timeout (default: 5)\n"
        << "  --expect-status <code>         Expected response status (default: 0)\n"
        << "  --expect-payload <payload>     Expected response payload\n"
        << "  --reconnect-per-request        Disable connection reuse\n"
        << "  --help                         Show this help message\n";
}

Args ParseArgs(int argc, char** argv) {
    Args args;
    for (int i = 1; i < argc; ++i) {
        const std::string flag = argv[i];
        auto require_value = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                throw std::invalid_argument(std::string("missing value for ") + name);
            }
            return argv[++i];
        };

        if (flag == "--host") {
            args.host = require_value("--host");
        } else if (flag == "--port") {
            args.port = static_cast<uint16_t>(std::stoul(require_value("--port")));
        } else if (flag == "--method") {
            args.method = require_value("--method");
        } else if (flag == "--payload") {
            args.payload = require_value("--payload");
        } else if (flag == "--connections") {
            args.connections = std::stoi(require_value("--connections"));
        } else if (flag == "--requests") {
            args.requests = std::stoull(require_value("--requests"));
        } else if (flag == "--duration") {
            args.duration = std::stod(require_value("--duration"));
        } else if (flag == "--timeout") {
            args.timeout = std::stod(require_value("--timeout"));
        } else if (flag == "--expect-status") {
            args.expect_status = static_cast<uint16_t>(std::stoul(require_value("--expect-status")));
        } else if (flag == "--expect-payload") {
            args.expect_payload = require_value("--expect-payload");
        } else if (flag == "--reconnect-per-request") {
            args.reconnect_per_request = true;
        } else if (flag == "--help" || flag == "-h") {
            PrintUsage(argv[0]);
            std::exit(0);
        } else {
            throw std::invalid_argument("unknown argument: " + flag);
        }
    }

    if (args.connections <= 0) {
        throw std::invalid_argument("--connections must be > 0");
    }
    if (args.duration <= 0.0 && args.requests == 0) {
        throw std::invalid_argument("--requests must be > 0 when --duration is not set");
    }
    if (args.port == 0) {
        throw std::invalid_argument("--port must be in 1..65535");
    }
    if (args.timeout <= 0.0) {
        throw std::invalid_argument("--timeout must be > 0");
    }

    return args;
}

std::string BuildRequest(uint64_t request_id, const std::string& method, const std::string& payload) {
    std::string packet;
    packet.reserve(kHeaderSize + method.size() + payload.size());
    AppendUint32(packet, kMagic);
    packet.push_back(static_cast<char>(kVersion));
    packet.push_back(static_cast<char>(kRequestType));
    AppendUint16(packet, 0);
    AppendUint32(packet, static_cast<uint32_t>(method.size()));
    AppendUint32(packet, static_cast<uint32_t>(payload.size()));
    AppendUint64(packet, request_id);
    packet.append(method);
    packet.append(payload);
    return packet;
}

void SendAll(int fd, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error(std::string("send failed: ") + std::strerror(errno));
        }
        sent += static_cast<size_t>(n);
    }
}

void RecvExact(int fd, char* buffer, size_t length) {
    size_t received = 0;
    while (received < length) {
        const ssize_t n = ::recv(fd, buffer + received, length - received, 0);
        if (n == 0) {
            throw std::runtime_error("connection closed by peer");
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error(std::string("recv failed: ") + std::strerror(errno));
        }
        received += static_cast<size_t>(n);
    }
}

Response ReadResponse(int fd) {
    char header[kHeaderSize];
    RecvExact(fd, header, sizeof(header));

    const uint32_t magic = LoadUint32(header);
    const uint8_t version = static_cast<uint8_t>(header[4]);
    const uint8_t type = static_cast<uint8_t>(header[5]);
    const uint16_t status = LoadUint16(header + 6);
    const uint32_t method_len = LoadUint32(header + 8);
    const uint32_t body_len = LoadUint32(header + 12);
    const uint64_t request_id = LoadUint64(header + 16);

    if (magic != kMagic) {
        throw std::runtime_error("invalid magic in response");
    }
    if (version != kVersion) {
        throw std::runtime_error("unsupported response version");
    }
    if (type != kResponseType) {
        throw std::runtime_error("unexpected response type");
    }
    if (method_len != 0) {
        throw std::runtime_error("unexpected response method length");
    }

    Response response;
    response.request_id = request_id;
    response.status = status;
    response.body.resize(body_len);
    if (body_len > 0) {
        RecvExact(fd, response.body.data(), body_len);
    }
    return response;
}

SocketHandle Connect(const Args& args) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* result = nullptr;
    const std::string port = std::to_string(args.port);
    const int rc = ::getaddrinfo(args.host.c_str(), port.c_str(), &hints, &result);
    if (rc != 0) {
        throw std::runtime_error(std::string("getaddrinfo failed: ") + gai_strerror(rc));
    }

    SocketHandle socket_fd;
    for (addrinfo* current = result; current != nullptr; current = current->ai_next) {
        SocketHandle candidate(::socket(current->ai_family, current->ai_socktype, current->ai_protocol));
        if (candidate.fd < 0) {
            continue;
        }

        timeval timeout{};
        timeout.tv_sec = static_cast<long>(args.timeout);
        timeout.tv_usec = static_cast<long>((args.timeout - timeout.tv_sec) * 1'000'000);
        ::setsockopt(candidate.fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        ::setsockopt(candidate.fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

        if (::connect(candidate.fd, current->ai_addr, current->ai_addrlen) == 0) {
            socket_fd = std::move(candidate);
            break;
        }
    }

    ::freeaddrinfo(result);

    if (socket_fd.fd < 0) {
        throw std::runtime_error("failed to connect to target");
    }
    return socket_fd;
}

double Percentile(const std::vector<double>& sorted_values, double ratio) {
    if (sorted_values.empty()) {
        return 0.0;
    }
    if (sorted_values.size() == 1) {
        return sorted_values.front();
    }
    const double position = ratio * static_cast<double>(sorted_values.size() - 1);
    const size_t lower = static_cast<size_t>(std::floor(position));
    const size_t upper = std::min(lower + 1, sorted_values.size() - 1);
    const double weight = position - static_cast<double>(lower);
    return sorted_values[lower] * (1.0 - weight) + sorted_values[upper] * weight;
}

void WorkerLoop(int worker_id, const Args& args, SharedState& state) {
    SocketHandle connection;

    auto close_connection = [&]() {
        if (connection.fd >= 0) {
            ::close(connection.release());
        }
    };

    try {
        while (true) {
            const std::optional<uint64_t> request_number = state.AcquireRequestIndex();
            if (!request_number.has_value()) {
                close_connection();
                return;
            }

            const uint64_t request_id =
                (static_cast<uint64_t>(worker_id) << 48) | request_number.value();
            const std::string packet = BuildRequest(request_id, args.method, args.payload);
            const auto started = std::chrono::steady_clock::now();

            try {
                if (args.reconnect_per_request) {
                    SocketHandle temp = Connect(args);
                    SendAll(temp.fd, packet);
                    Response response = ReadResponse(temp.fd);

                    const double latency_ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - started).count();

                    if (response.request_id != request_id) {
                        throw std::runtime_error("request_id mismatch");
                    }
                    if (response.status != args.expect_status) {
                        throw std::runtime_error("unexpected status: " + std::to_string(response.status));
                    }
                    if (args.expect_payload.has_value() && response.body != args.expect_payload.value()) {
                        throw std::runtime_error("unexpected payload");
                    }

                    state.RecordSuccess(latency_ms, packet.size(), kHeaderSize + response.body.size());
                } else {
                    if (connection.fd < 0) {
                        connection = Connect(args);
                    }
                    SendAll(connection.fd, packet);
                    Response response = ReadResponse(connection.fd);

                    const double latency_ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - started).count();

                    if (response.request_id != request_id) {
                        throw std::runtime_error("request_id mismatch");
                    }
                    if (response.status != args.expect_status) {
                        throw std::runtime_error("unexpected status: " + std::to_string(response.status));
                    }
                    if (args.expect_payload.has_value() && response.body != args.expect_payload.value()) {
                        throw std::runtime_error("unexpected payload");
                    }

                    state.RecordSuccess(latency_ms, packet.size(), kHeaderSize + response.body.size());
                }
            } catch (const std::exception& ex) {
                state.RecordFailure(
                    "worker " + std::to_string(worker_id) + " request failed: " + ex.what());
                close_connection();
                return;
            }
        }
    } catch (const std::exception& ex) {
        state.RecordFailure("worker " + std::to_string(worker_id) + " fatal error: " + ex.what());
        close_connection();
    }
}

int PrintReport(const Args& args, const SharedState& state) {
    const double elapsed = std::max(
        std::chrono::duration<double>(std::chrono::steady_clock::now() - state.start).count(),
        1e-9);

    std::vector<double> latencies;
    {
        std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(state.latencies_mutex));
        latencies = state.latencies_ms;
    }
    std::sort(latencies.begin(), latencies.end());

    const uint64_t success = state.success.load(std::memory_order_relaxed);
    const uint64_t failures = state.failures.load(std::memory_order_relaxed);
    const uint64_t total_completed = success + failures;
    const double avg_latency = latencies.empty()
        ? 0.0
        : std::accumulate(latencies.begin(), latencies.end(), 0.0) / latencies.size();

    std::cout << "Benchmark finished\n";
    std::cout << "Target       : " << args.host << ":" << args.port << "\n";
    std::cout << "Method       : " << args.method << "\n";
    std::cout << "Payload size : " << args.payload.size() << " bytes\n";
    std::cout << "Connections  : " << args.connections << "\n";
    std::cout << std::fixed << std::setprecision(3);
    std::cout << "Elapsed      : " << elapsed << " s\n";
    std::cout << "Completed    : " << total_completed << "\n";
    std::cout << "Success      : " << success << "\n";
    std::cout << "Failures     : " << failures << "\n";
    std::cout << std::setprecision(2);
    std::cout << "QPS          : " << static_cast<double>(success) / elapsed << "\n";
    std::cout << "Tx throughput: "
              << static_cast<double>(state.bytes_sent.load(std::memory_order_relaxed)) / elapsed / 1024.0
              << " KiB/s\n";
    std::cout << "Rx throughput: "
              << static_cast<double>(state.bytes_received.load(std::memory_order_relaxed)) / elapsed / 1024.0
              << " KiB/s\n";

    if (!latencies.empty()) {
        std::cout << std::setprecision(3);
        std::cout << "Latency avg  : " << avg_latency << " ms\n";
        std::cout << "Latency min  : " << latencies.front() << " ms\n";
        std::cout << "Latency p50  : " << Percentile(latencies, 0.50) << " ms\n";
        std::cout << "Latency p99  : " << Percentile(latencies, 0.99) << " ms\n";
        std::cout << "Latency max  : " << latencies.back() << " ms\n";
    }

    if (!state.first_error.empty()) {
        std::cout << "First error  : " << state.first_error << "\n";
    }

    return failures == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const Args args = ParseArgs(argc, argv);
        SharedState state(args);

        std::vector<std::thread> workers;
        workers.reserve(static_cast<size_t>(args.connections));
        for (int i = 0; i < args.connections; ++i) {
            workers.emplace_back(WorkerLoop, i + 1, std::cref(args), std::ref(state));
        }

        for (std::thread& worker : workers) {
            worker.join();
        }

        return PrintReport(args, state);
    } catch (const std::exception& ex) {
        std::cerr << "benchmark client error: " << ex.what() << "\n";
        return 1;
    }
}
