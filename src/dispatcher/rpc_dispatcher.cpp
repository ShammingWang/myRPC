#include "dispatcher/rpc_dispatcher.h"

#include <algorithm>
#include <exception>

#include "codec/rpc_serialization.h"

void RpcDispatcher::RegisterMethod(std::string service, std::string method, MethodHandler handler) {
    handlers_[BuildMethodKey(service, method)] = std::move(handler);
}

void RpcDispatcher::RegisterService(const RpcService& service) {
    service.RegisterMethods(*this);
}

RpcResponse RpcDispatcher::Dispatch(const RpcRequest& request) const {
    RpcResponse response;
    response.request_id = request.request_id;
    response.serialization = request.serialization;

    const auto it = handlers_.find(request.method);
    if (it == handlers_.end()) {
        response.status_code = RpcStatusCode::kMethodNotFound;
        response.payload = "method not found: " + request.method;
        return response;
    }

    std::string decoded_payload;
    std::string error;
    if (!RpcSerializerRegistry::Deserialize(request.serialization, request.payload, decoded_payload,
                                            error)) {
        response.status_code = RpcStatusCode::kSerializationError;
        response.payload = error;
        return response;
    }

    try {
        std::string handler_result = it->second(decoded_payload);
        std::string encoded_payload;
        if (!RpcSerializerRegistry::Serialize(request.serialization, handler_result, encoded_payload,
                                            error)) {
            response.status_code = RpcStatusCode::kSerializationError;
            response.payload = error;
            return response;
        }

        response.status_code = RpcStatusCode::kOk;
        response.payload = std::move(encoded_payload);
    } catch (const std::exception& ex) {
        response.status_code = RpcStatusCode::kHandlerException;
        response.payload = std::string("handler threw exception: ") + ex.what();
    } catch (...) {
        response.status_code = RpcStatusCode::kHandlerException;
        response.payload = "handler threw unknown exception";
    }

    return response;
}

std::vector<std::string> RpcDispatcher::ListMethods() const {
    std::vector<std::string> methods;
    methods.reserve(handlers_.size());
    for (const auto& [method, _] : handlers_) {
        methods.push_back(method);
    }
    std::sort(methods.begin(), methods.end());
    return methods;
}

std::string RpcDispatcher::BuildMethodKey(const std::string& service, const std::string& method) {
    if (service.empty()) {
        return method;
    }
    return service + "." + method;
}
