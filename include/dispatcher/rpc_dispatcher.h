#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "codec/rpc_message.h"

class RpcDispatcher;

class RpcService {
public:
    virtual ~RpcService() = default;

    virtual std::string ServiceName() const = 0;
    virtual void RegisterMethods(RpcDispatcher& dispatcher) const = 0;
};

class RpcDispatcher {
public:
    using MethodHandler = std::function<std::string(const std::string&)>;

    void RegisterMethod(std::string service, std::string method, MethodHandler handler);
    void RegisterService(const RpcService& service);
    RpcResponse Dispatch(const RpcRequest& request) const;
    std::vector<std::string> ListMethods() const;

private:
    static std::string BuildMethodKey(const std::string& service, const std::string& method);

    std::unordered_map<std::string, MethodHandler> handlers_;
};
