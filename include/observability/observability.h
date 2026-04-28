#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "codec/rpc_message.h"

struct ObservabilityOptions {
    std::chrono::milliseconds slow_request_threshold{50};
    bool enable_request_trace = true;
    bool trace_all_requests = false;
};

class Observability {
public:
    explicit Observability(ObservabilityOptions options = {});

    void RegisterIoThread(size_t io_thread_index);
    void OnConnectionAccepted();
    void OnConnectionClosed();
    void OnRequestDecoded(const RpcRequest& request);
    void OnRequestDispatched();
    void OnWorkerPoolRejected();
    void OnProtocolError();
    void SetIoThreadConnectionCount(size_t io_thread_index, size_t connection_count);
    void SetIoThreadPendingResponseCount(size_t io_thread_index, size_t pending_response_count);
    void OnIoThreadWakeup(size_t io_thread_index);
    void OnIoThreadLoopIteration(size_t io_thread_index, size_t ready_events,
                                 std::chrono::microseconds processing_time);
    void OnIoThreadWrite(size_t io_thread_index, size_t bytes_written,
                         size_t write_calls, size_t eagain_count);
    void OnResponseSent(const RpcRequest& request, const RpcResponse& response,
                        std::chrono::steady_clock::time_point worker_finished_at,
                        std::chrono::steady_clock::time_point response_enqueued_at,
                        std::chrono::steady_clock::time_point io_processing_started_at,
                        std::chrono::steady_clock::time_point response_sent_at);
    std::string ExportMetrics() const;

private:
    struct Snapshot {
        uint64_t accepted_connections = 0;
        uint64_t active_connections = 0;
        uint64_t total_requests = 0;
        uint64_t inflight_requests = 0;
        uint64_t completed_requests = 0;
        uint64_t success_responses = 0;
        uint64_t error_responses = 0;
        uint64_t timeout_responses = 0;
        uint64_t overload_responses = 0;
        uint64_t protocol_errors = 0;
        uint64_t worker_rejections = 0;
        uint64_t queue_latency_us_total = 0;
        uint64_t queue_latency_us_max = 0;
        uint64_t handler_latency_us_total = 0;
        uint64_t handler_latency_us_max = 0;
        uint64_t return_queue_latency_us_total = 0;
        uint64_t return_queue_latency_us_max = 0;
        uint64_t send_latency_us_total = 0;
        uint64_t send_latency_us_max = 0;
        uint64_t io_return_latency_us_total = 0;
        uint64_t io_return_latency_us_max = 0;
        uint64_t end_to_end_latency_us_total = 0;
        uint64_t end_to_end_latency_us_max = 0;
    };

    struct MethodSnapshot {
        std::string method;
        uint64_t total_requests = 0;
        uint64_t inflight_requests = 0;
        uint64_t completed_requests = 0;
        uint64_t success_responses = 0;
        uint64_t error_responses = 0;
        uint64_t timeout_responses = 0;
        uint64_t overload_responses = 0;
        uint64_t queue_latency_us_total = 0;
        uint64_t queue_latency_us_max = 0;
        uint64_t handler_latency_us_total = 0;
        uint64_t handler_latency_us_max = 0;
        uint64_t return_queue_latency_us_total = 0;
        uint64_t return_queue_latency_us_max = 0;
        uint64_t send_latency_us_total = 0;
        uint64_t send_latency_us_max = 0;
        uint64_t io_return_latency_us_total = 0;
        uint64_t io_return_latency_us_max = 0;
        uint64_t end_to_end_latency_us_total = 0;
        uint64_t end_to_end_latency_us_max = 0;
    };

    struct IoThreadSnapshot {
        size_t io_thread_index = 0;
        uint64_t connection_count = 0;
        uint64_t pending_response_count = 0;
        uint64_t wakeups = 0;
        uint64_t loop_iterations = 0;
        uint64_t ready_events_last = 0;
        uint64_t ready_events_max = 0;
        uint64_t loop_processing_us_total = 0;
        uint64_t loop_processing_us_max = 0;
        uint64_t write_calls = 0;
        uint64_t write_bytes = 0;
        uint64_t write_eagain = 0;
    };

    Snapshot LoadSnapshot() const;
    std::vector<MethodSnapshot> LoadMethodSnapshots() const;
    std::vector<IoThreadSnapshot> LoadIoThreadSnapshots() const;
    bool ShouldTrace(const RpcRequest& request, const RpcResponse& response,
                     std::chrono::milliseconds end_to_end_latency) const;
    void LogTrace(const RpcRequest& request, const RpcResponse& response,
                  std::chrono::milliseconds queue_latency,
                  std::chrono::milliseconds handler_latency,
                  std::chrono::milliseconds io_return_latency,
                  std::chrono::milliseconds end_to_end_latency) const;

    struct MethodStats {
        uint64_t total_requests = 0;
        uint64_t inflight_requests = 0;
        uint64_t completed_requests = 0;
        uint64_t success_responses = 0;
        uint64_t error_responses = 0;
        uint64_t timeout_responses = 0;
        uint64_t overload_responses = 0;
        uint64_t queue_latency_us_total = 0;
        uint64_t queue_latency_us_max = 0;
        uint64_t handler_latency_us_total = 0;
        uint64_t handler_latency_us_max = 0;
        uint64_t return_queue_latency_us_total = 0;
        uint64_t return_queue_latency_us_max = 0;
        uint64_t send_latency_us_total = 0;
        uint64_t send_latency_us_max = 0;
        uint64_t io_return_latency_us_total = 0;
        uint64_t io_return_latency_us_max = 0;
        uint64_t end_to_end_latency_us_total = 0;
        uint64_t end_to_end_latency_us_max = 0;
    };

    struct IoThreadStats {
        std::atomic<uint64_t> connection_count{0};
        std::atomic<uint64_t> pending_response_count{0};
        std::atomic<uint64_t> wakeups{0};
        std::atomic<uint64_t> loop_iterations{0};
        std::atomic<uint64_t> ready_events_last{0};
        std::atomic<uint64_t> ready_events_max{0};
        std::atomic<uint64_t> loop_processing_us_total{0};
        std::atomic<uint64_t> loop_processing_us_max{0};
        std::atomic<uint64_t> write_calls{0};
        std::atomic<uint64_t> write_bytes{0};
        std::atomic<uint64_t> write_eagain{0};
    };

    ObservabilityOptions options_;
    std::atomic<uint64_t> accepted_connections_{0};
    std::atomic<uint64_t> active_connections_{0};
    std::atomic<uint64_t> total_requests_{0};
    std::atomic<uint64_t> inflight_requests_{0};
    std::atomic<uint64_t> completed_requests_{0};
    std::atomic<uint64_t> success_responses_{0};
    std::atomic<uint64_t> error_responses_{0};
    std::atomic<uint64_t> timeout_responses_{0};
    std::atomic<uint64_t> overload_responses_{0};
    std::atomic<uint64_t> protocol_errors_{0};
    std::atomic<uint64_t> worker_rejections_{0};
    std::atomic<uint64_t> queue_latency_us_total_{0};
    std::atomic<uint64_t> queue_latency_us_max_{0};
    std::atomic<uint64_t> handler_latency_us_total_{0};
    std::atomic<uint64_t> handler_latency_us_max_{0};
    std::atomic<uint64_t> return_queue_latency_us_total_{0};
    std::atomic<uint64_t> return_queue_latency_us_max_{0};
    std::atomic<uint64_t> send_latency_us_total_{0};
    std::atomic<uint64_t> send_latency_us_max_{0};
    std::atomic<uint64_t> io_return_latency_us_total_{0};
    std::atomic<uint64_t> io_return_latency_us_max_{0};
    std::atomic<uint64_t> end_to_end_latency_us_total_{0};
    std::atomic<uint64_t> end_to_end_latency_us_max_{0};
    mutable std::mutex method_stats_mutex_;
    std::unordered_map<std::string, MethodStats> method_stats_;
    mutable std::mutex io_thread_stats_mutex_;
    std::unordered_map<size_t, IoThreadStats> io_thread_stats_;
    mutable std::mutex log_mutex_;
    std::chrono::steady_clock::time_point start_time_;
};
