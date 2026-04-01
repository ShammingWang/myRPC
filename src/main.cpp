#include "server.h"

#include <algorithm>
#include <csignal>
#include <cctype>
#include <cstdint>
#include <iostream>
#include <string>

namespace {

constexpr uint16_t kDefaultPort = 8080;

std::string ToUpper(std::string input) {
    std::transform(input.begin(), input.end(), input.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return input;
}

}  // namespace

int main() {
    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);

    Server server(kDefaultPort);
    server.RegisterHandler("echo", [](const std::string& payload) { return payload; });
    server.RegisterHandler("uppercase", [](const std::string& payload) { return ToUpper(payload); });

    if (!server.Start()) {
        return 1;
    }

    std::cout << "registered RPC methods: echo, uppercase\n";
    server.Run();
    server.Stop();
    std::cout << "server stopped\n";
    return 0;
}
