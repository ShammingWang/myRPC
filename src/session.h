#pragma once

#include <string>

class Session {
public:
    Session(int conn_fd, std::string client_label);

    void OnConnected() const;
    bool OnReadable();
    bool OnWritable();
    bool WantsWrite() const;
    bool ShouldClose() const;
    void OnClosed() const;

private:
    bool DrainReads();
    bool DrainWrites();
    void ProcessRequests();
    std::string BuildResponse(const std::string& request) const;

    int conn_fd_;
    std::string client_label_;
    std::string pending_data_;
    std::string pending_write_;
    bool peer_closed_ = false;
};
