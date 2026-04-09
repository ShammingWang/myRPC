#include "client/rpc_client.h"

#include <iostream>

int main() {
    RpcClientOptions options;
    options.host = "127.0.0.1";
    options.port = 8080;
    options.connect_timeout_ms = 3000;

    RpcClient client(options);

    RpcResponse echo = client.Call("EchoService", "Echo", "hello rpc",
                                   RpcCallOptions{1000, 0, RpcSerializationType::kRaw});
    std::cout << "echo status=" << static_cast<uint16_t>(echo.status_code)
              << " payload=" << echo.payload << "\n";

    RpcResponse uppercase = client.Call("EchoService", "Uppercase", "hello again",
                                        RpcCallOptions{1000, 0, RpcSerializationType::kRaw});
    std::cout << "uppercase status=" << static_cast<uint16_t>(uppercase.status_code)
              << " payload=" << uppercase.payload << "\n";

    RpcResponse timeout = client.Call("EchoService", "SlowEcho", "slow request",
                                      RpcCallOptions{50, 400, RpcSerializationType::kRaw});
    std::cout << "slow_echo status=" << static_cast<uint16_t>(timeout.status_code)
              << " payload=" << timeout.payload << "\n";

    client.Close();
    return 0;
}
