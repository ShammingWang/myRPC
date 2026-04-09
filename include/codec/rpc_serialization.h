#pragma once

#include <string>

#include "codec/rpc_message.h"

class RpcSerializerRegistry {
public:
    static bool Serialize(RpcSerializationType type, const std::string& input, std::string& output,
                          std::string& error);
    static bool Deserialize(RpcSerializationType type, const std::string& input,
                            std::string& output, std::string& error);
    static std::string ToString(RpcSerializationType type);

private:
    static bool ValidateJson(const std::string& input, std::string& error);
};
