#include "codec/rpc_serialization.h"

#include <cctype>

bool RpcSerializerRegistry::Serialize(RpcSerializationType type, const std::string& input,
                                      std::string& output, std::string& error) {
    output = input;
    error.clear();
    if (type == RpcSerializationType::kRaw) {
        return true;
    }
    if (type == RpcSerializationType::kJson) {
        return ValidateJson(input, error);
    }

    error = "unsupported serialization type";
    return false;
}

bool RpcSerializerRegistry::Deserialize(RpcSerializationType type, const std::string& input,
                                        std::string& output, std::string& error) {
    output = input;
    error.clear();
    if (type == RpcSerializationType::kRaw) {
        return true;
    }
    if (type == RpcSerializationType::kJson) {
        return ValidateJson(input, error);
    }

    error = "unsupported serialization type";
    return false;
}

std::string RpcSerializerRegistry::ToString(RpcSerializationType type) {
    switch (type) {
        case RpcSerializationType::kRaw:
            return "raw";
        case RpcSerializationType::kJson:
            return "json";
    }

    return "unknown";
}

bool RpcSerializerRegistry::ValidateJson(const std::string& input, std::string& error) {
    error.clear();
    const auto first = input.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        error = "json payload is empty";
        return false;
    }

    const auto last = input.find_last_not_of(" \t\r\n");
    const char first_ch = input[first];
    const char last_ch = input[last];
    const bool is_object = first_ch == '{' && last_ch == '}';
    const bool is_array = first_ch == '[' && last_ch == ']';
    const bool is_string = first_ch == '"' && last_ch == '"';
    const bool is_keyword =
        input.compare(first, 4, "true") == 0 || input.compare(first, 5, "false") == 0 ||
        input.compare(first, 4, "null") == 0;
    const bool is_number = first_ch == '-' || std::isdigit(static_cast<unsigned char>(first_ch)) != 0;

    if (is_object || is_array || is_string || is_keyword || is_number) {
        return true;
    }

    error = "json payload does not look like a valid JSON literal";
    return false;
}
