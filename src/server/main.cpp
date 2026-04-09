#include "server/server.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cctype>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>

namespace {

constexpr uint16_t kDefaultPort = 8080;

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

    Server server(kDefaultPort, options);
    EchoService echo_service;
    server.RegisterService(echo_service);

    if (!server.Start()) {
        return 1;
    }

    std::cout << "registered RPC methods: EchoService.Echo, EchoService.Uppercase, "
                "EchoService.SlowEcho\n";
    server.Run();
    server.Stop();
    std::cout << "server stopped\n";
    return 0;
}
