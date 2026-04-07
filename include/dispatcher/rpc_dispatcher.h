#pragma once

#include <functional>
#include <string>
#include <unordered_map>

#include "codec/rpc_message.h"

class RpcDispatcher {
public:
    using Handler = std::function<std::string(const std::string&)>;

    void RegisterHandler(std::string method, Handler handler);
    RpcResponse Dispatch(const RpcRequest& request) const;

private:
    std::unordered_map<std::string, Handler> handlers_;
};
