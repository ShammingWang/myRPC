#include "dispatcher/rpc_dispatcher.h"

#include <exception>

void RpcDispatcher::RegisterHandler(std::string method, Handler handler) {
    handlers_[std::move(method)] = std::move(handler);
}

RpcResponse RpcDispatcher::Dispatch(const RpcRequest& request) const {
    RpcResponse response;
    response.request_id = request.request_id;

    const auto it = handlers_.find(request.method);
    if (it == handlers_.end()) {
        response.status_code = 404;
        response.payload = "method not found: " + request.method;
        return response;
    }

    try {
        response.status_code = 0;
        response.payload = it->second(request.payload);
    } catch (const std::exception& ex) {
        response.status_code = 500;
        response.payload = std::string("handler threw exception: ") + ex.what();
    } catch (...) {
        response.status_code = 500;
        response.payload = "handler threw unknown exception";
    }

    return response;
}
