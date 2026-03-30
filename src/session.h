#pragma once

#include <string>

class Session {
public:
    Session(int conn_fd, std::string client_label);

    void Run();

private:
    bool ReadOnce(std::string& request);
    bool WriteAll(const std::string& response);
    std::string BuildResponse() const;

    int conn_fd_;
    std::string client_label_;
};
