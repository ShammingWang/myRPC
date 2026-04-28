#include "server/server.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

namespace {

constexpr uint16_t kDefaultPort = 8080;
volatile uint64_t g_cpu_sink = 0;

size_t ReadIoThreadCountFromEnv() {
    const char* value = std::getenv("MRPC_IO_THREADS");
    if (value == nullptr || *value == '\0') {
        return 0;
    }

    try {
        return static_cast<size_t>(std::stoull(value));
    } catch (...) {
        std::cerr << "invalid MRPC_IO_THREADS value: " << value << '\n';
        return 0;
    }
}

uint64_t ReadUint64FromEnv(const char* name, uint64_t default_value) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return default_value;
    }

    try {
        return static_cast<uint64_t>(std::stoull(value));
    } catch (...) {
        std::cerr << "invalid " << name << " value: " << value << '\n';
        return default_value;
    }
}

bool ReadBoolFromEnv(const char* name, bool default_value) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return default_value;
    }

    const std::string input(value);
    if (input == "1" || input == "true" || input == "TRUE") {
        return true;
    }
    if (input == "0" || input == "false" || input == "FALSE") {
        return false;
    }

    std::cerr << "invalid " << name << " value: " << value << '\n';
    return default_value;
}

std::string ToUpper(std::string input) {
    std::transform(input.begin(), input.end(), input.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return input;
}

class EchoService final : public RpcService {
public:
    std::string ServiceName() const override {
        return "EchoService";
    }

    void RegisterMethods(RpcDispatcher& dispatcher) const override {
        dispatcher.RegisterMethod(ServiceName(), "Echo",
                                  [](const std::string& payload) { return payload; });
        dispatcher.RegisterMethod(ServiceName(), "Uppercase",
                                  [](const std::string& payload) { return ToUpper(payload); });
        dispatcher.RegisterMethod(ServiceName(), "SlowEcho", [](const std::string& payload) {
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            return payload;
        });
        dispatcher.RegisterMethod(ServiceName(), "CpuHeavy", [](const std::string& payload) {
            uint64_t acc = 1469598103934665603ull;
            for (int round = 0; round < 2000; ++round) {
                for (const unsigned char ch : payload) {
                    acc ^= static_cast<uint64_t>(ch) + static_cast<uint64_t>(round);
                    acc *= 1099511628211ull;
                }
            }
            g_cpu_sink = acc;
            return payload;
        });
    }
};

}  // namespace

int main() {
    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);

    ServerOptions options;
    options.idle_connection_timeout = std::chrono::seconds(30);
    options.max_pending_requests_per_connection = 256;
    options.max_outbound_buffer_bytes_per_connection = 512 * 1024;
    options.io_thread_count = ReadIoThreadCountFromEnv();
    options.admin_port = static_cast<uint16_t>(ReadUint64FromEnv("MRPC_ADMIN_PORT", 9090));
    options.slow_request_threshold = std::chrono::milliseconds(
        ReadUint64FromEnv("MRPC_SLOW_REQUEST_MS", 50));
    options.enable_request_trace = ReadBoolFromEnv("MRPC_ENABLE_REQUEST_TRACE", true);
    options.trace_all_requests = ReadBoolFromEnv("MRPC_TRACE_ALL_REQUESTS", false);

    Server server(kDefaultPort, options);
    EchoService echo_service;
    server.RegisterService(echo_service);

    if (!server.Start()) {
        return 1;
    }

    std::cout << "registered RPC methods: EchoService.Echo, EchoService.Uppercase, "
                 "EchoService.SlowEcho, EchoService.CpuHeavy\n";
    if (options.admin_port != 0) {
        std::cout << "admin metrics endpoint: http://0.0.0.0:" << options.admin_port
                  << "/metrics\n";
    }
    server.Run();
    server.Stop();
    std::cout << "server stopped\n";
    return 0;
}
