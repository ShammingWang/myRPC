#include "server.h"

#include <csignal>
#include <cstdint>
#include <iostream>

namespace {

constexpr uint16_t kDefaultPort = 8080;

}  // namespace

int main() {
    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);

    Server server(kDefaultPort);
    if (!server.Start()) {
        return 1;
    }

    server.Run();
    server.Stop();
    std::cout << "server stopped\n";
    return 0;
}
