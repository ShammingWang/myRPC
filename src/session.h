#pragma once

#include <string>

class Session {
public:
    Session(int conn_fd, std::string client_label);

    void Run();

private:
    bool ReadLine(std::string& line);
    bool WriteAll(const std::string& response);
    std::string BuildResponse(const std::string& request) const;

    int conn_fd_;
    std::string client_label_;
    std::string pending_data_;
};
