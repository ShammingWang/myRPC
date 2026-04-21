#include "observability/observability.h"

#include <algorithm>
#include <iostream>
#include <sstream>

namespace {

using Clock = std::chrono::steady_clock;

int64_t ToMilliseconds(Clock::duration duration) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
}

int64_t ToMicroseconds(Clock::duration duration) {
    return std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
}

double MicrosecondsToMilliseconds(uint64_t microseconds) {
    return static_cast<double>(microseconds) / 1000.0;
}

void UpdateMax(std::atomic<uint64_t>& target, uint64_t candidate) {
    uint64_t current = target.load(std::memory_order_relaxed);
    while (current < candidate &&
           !target.compare_exchange_weak(current, candidate, std::memory_order_relaxed)) {
    }
}

std::string EscapePrometheusLabelValue(const std::string& input) {
    std::ostringstream out;
    for (const char ch : input) {
        switch (ch) {
            case '\\':
                out << "\\\\";
                break;
            case '"':
                out << "\\\"";
                break;
            case '\n':
                out << "\\n";
                break;
            default:
                out << ch;
                break;
        }
    }
    return out.str();
}

}  // namespace

Observability::Observability(ObservabilityOptions options)
    : options_(std::move(options)), start_time_(Clock::now()) {}

void Observability::OnConnectionAccepted() {
    accepted_connections_.fetch_add(1, std::memory_order_relaxed);
    active_connections_.fetch_add(1, std::memory_order_relaxed);
}

void Observability::OnConnectionClosed() {
    active_connections_.fetch_sub(1, std::memory_order_relaxed);
}

void Observability::OnRequestDecoded(const RpcRequest& request) {
    total_requests_.fetch_add(1, std::memory_order_relaxed);
    inflight_requests_.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(method_stats_mutex_);
    MethodStats& stats = method_stats_[request.method];
    stats.total_requests += 1;
    stats.inflight_requests += 1;
}

void Observability::OnRequestDispatched() {}

void Observability::OnWorkerPoolRejected() {
    worker_rejections_.fetch_add(1, std::memory_order_relaxed);
}

void Observability::OnProtocolError() {
    protocol_errors_.fetch_add(1, std::memory_order_relaxed);
}

void Observability::OnResponseSent(const RpcRequest& request, const RpcResponse& response,
                                   Clock::time_point worker_finished_at,
                                   Clock::time_point response_enqueued_at,
                                   Clock::time_point response_sent_at) {
    completed_requests_.fetch_add(1, std::memory_order_relaxed);
    inflight_requests_.fetch_sub(1, std::memory_order_relaxed);

    if (IsSuccess(response.status_code)) {
        success_responses_.fetch_add(1, std::memory_order_relaxed);
    } else {
        error_responses_.fetch_add(1, std::memory_order_relaxed);
    }

    if (response.status_code == RpcStatusCode::kRequestTimeout) {
        timeout_responses_.fetch_add(1, std::memory_order_relaxed);
    }
    if (response.status_code == RpcStatusCode::kServerOverloaded) {
        overload_responses_.fetch_add(1, std::memory_order_relaxed);
    }

    const auto queue_latency = std::chrono::duration_cast<std::chrono::milliseconds>(
        request.worker_started_at - request.enqueued_at);
    const auto handler_latency = std::chrono::duration_cast<std::chrono::milliseconds>(
        worker_finished_at - request.worker_started_at);
    const auto io_return_latency = std::chrono::duration_cast<std::chrono::milliseconds>(
        response_sent_at - response_enqueued_at);
    const auto end_to_end_latency = std::chrono::duration_cast<std::chrono::milliseconds>(
        response_sent_at - request.received_at);
    const uint64_t queue_latency_us = static_cast<uint64_t>(
        std::max<int64_t>(0, ToMicroseconds(request.worker_started_at - request.enqueued_at)));
    const uint64_t handler_latency_us = static_cast<uint64_t>(
        std::max<int64_t>(0, ToMicroseconds(worker_finished_at - request.worker_started_at)));
    const uint64_t io_return_latency_us = static_cast<uint64_t>(
        std::max<int64_t>(0, ToMicroseconds(response_sent_at - response_enqueued_at)));
    const uint64_t end_to_end_latency_us = static_cast<uint64_t>(
        std::max<int64_t>(0, ToMicroseconds(response_sent_at - request.received_at)));

    queue_latency_us_total_.fetch_add(queue_latency_us, std::memory_order_relaxed);
    handler_latency_us_total_.fetch_add(handler_latency_us, std::memory_order_relaxed);
    io_return_latency_us_total_.fetch_add(io_return_latency_us, std::memory_order_relaxed);
    end_to_end_latency_us_total_.fetch_add(end_to_end_latency_us, std::memory_order_relaxed);
    UpdateMax(queue_latency_us_max_, queue_latency_us);
    UpdateMax(handler_latency_us_max_, handler_latency_us);
    UpdateMax(io_return_latency_us_max_, io_return_latency_us);
    UpdateMax(end_to_end_latency_us_max_, end_to_end_latency_us);

    {
        std::lock_guard<std::mutex> lock(method_stats_mutex_);
        MethodStats& stats = method_stats_[request.method];
        if (stats.inflight_requests > 0) {
            stats.inflight_requests -= 1;
        }
        stats.completed_requests += 1;
        if (IsSuccess(response.status_code)) {
            stats.success_responses += 1;
        } else {
            stats.error_responses += 1;
        }
        if (response.status_code == RpcStatusCode::kRequestTimeout) {
            stats.timeout_responses += 1;
        }
        if (response.status_code == RpcStatusCode::kServerOverloaded) {
            stats.overload_responses += 1;
        }
        stats.queue_latency_us_total += queue_latency_us;
        stats.handler_latency_us_total += handler_latency_us;
        stats.io_return_latency_us_total += io_return_latency_us;
        stats.end_to_end_latency_us_total += end_to_end_latency_us;
        stats.queue_latency_us_max = std::max(stats.queue_latency_us_max, queue_latency_us);
        stats.handler_latency_us_max =
            std::max(stats.handler_latency_us_max, handler_latency_us);
        stats.io_return_latency_us_max =
            std::max(stats.io_return_latency_us_max, io_return_latency_us);
        stats.end_to_end_latency_us_max =
            std::max(stats.end_to_end_latency_us_max, end_to_end_latency_us);
    }

    if (ShouldTrace(request, response, end_to_end_latency)) {
        LogTrace(request, response, queue_latency, handler_latency, io_return_latency,
                 end_to_end_latency);
    }
}

std::string Observability::ExportMetrics() const {
    const auto now = Clock::now();
    const Snapshot snapshot = LoadSnapshot();
    const double uptime_seconds = std::max(0.001, ToMilliseconds(now - start_time_) / 1000.0);
    const double average_qps = static_cast<double>(snapshot.completed_requests) / uptime_seconds;

    std::ostringstream out;
    out << "# TYPE mrpc_uptime_seconds gauge\n";
    out << "mrpc_uptime_seconds " << uptime_seconds << '\n';
    out << "# TYPE mrpc_connections_accepted_total counter\n";
    out << "mrpc_connections_accepted_total " << snapshot.accepted_connections << '\n';
    out << "# TYPE mrpc_connections_active gauge\n";
    out << "mrpc_connections_active " << snapshot.active_connections << '\n';
    out << "# TYPE mrpc_requests_total counter\n";
    out << "mrpc_requests_total " << snapshot.total_requests << '\n';
    out << "# TYPE mrpc_requests_inflight gauge\n";
    out << "mrpc_requests_inflight " << snapshot.inflight_requests << '\n';
    out << "# TYPE mrpc_requests_completed_total counter\n";
    out << "mrpc_requests_completed_total " << snapshot.completed_requests << '\n';
    out << "# TYPE mrpc_responses_success_total counter\n";
    out << "mrpc_responses_success_total " << snapshot.success_responses << '\n';
    out << "# TYPE mrpc_responses_error_total counter\n";
    out << "mrpc_responses_error_total " << snapshot.error_responses << '\n';
    out << "# TYPE mrpc_responses_timeout_total counter\n";
    out << "mrpc_responses_timeout_total " << snapshot.timeout_responses << '\n';
    out << "# TYPE mrpc_responses_overload_total counter\n";
    out << "mrpc_responses_overload_total " << snapshot.overload_responses << '\n';
    out << "# TYPE mrpc_protocol_errors_total counter\n";
    out << "mrpc_protocol_errors_total " << snapshot.protocol_errors << '\n';
    out << "# TYPE mrpc_worker_rejections_total counter\n";
    out << "mrpc_worker_rejections_total " << snapshot.worker_rejections << '\n';
    out << "# TYPE mrpc_completed_requests_average_qps gauge\n";
    out << "mrpc_completed_requests_average_qps " << average_qps << '\n';
    const double completed = std::max<uint64_t>(snapshot.completed_requests, 1);
    out << "# TYPE mrpc_latency_queue_average_ms gauge\n";
    out << "mrpc_latency_queue_average_ms "
        << MicrosecondsToMilliseconds(snapshot.queue_latency_us_total) / completed << '\n';
    out << "# TYPE mrpc_latency_queue_max_ms gauge\n";
    out << "mrpc_latency_queue_max_ms "
        << MicrosecondsToMilliseconds(snapshot.queue_latency_us_max) << '\n';
    out << "# TYPE mrpc_latency_handler_average_ms gauge\n";
    out << "mrpc_latency_handler_average_ms "
        << MicrosecondsToMilliseconds(snapshot.handler_latency_us_total) / completed << '\n';
    out << "# TYPE mrpc_latency_handler_max_ms gauge\n";
    out << "mrpc_latency_handler_max_ms "
        << MicrosecondsToMilliseconds(snapshot.handler_latency_us_max) << '\n';
    out << "# TYPE mrpc_latency_io_return_average_ms gauge\n";
    out << "mrpc_latency_io_return_average_ms "
        << MicrosecondsToMilliseconds(snapshot.io_return_latency_us_total) / completed << '\n';
    out << "# TYPE mrpc_latency_io_return_max_ms gauge\n";
    out << "mrpc_latency_io_return_max_ms "
        << MicrosecondsToMilliseconds(snapshot.io_return_latency_us_max) << '\n';
    out << "# TYPE mrpc_latency_end_to_end_average_ms gauge\n";
    out << "mrpc_latency_end_to_end_average_ms "
        << MicrosecondsToMilliseconds(snapshot.end_to_end_latency_us_total) / completed << '\n';
    out << "# TYPE mrpc_latency_end_to_end_max_ms gauge\n";
    out << "mrpc_latency_end_to_end_max_ms "
        << MicrosecondsToMilliseconds(snapshot.end_to_end_latency_us_max) << '\n';

    const std::vector<MethodSnapshot> methods = LoadMethodSnapshots();
    for (const MethodSnapshot& method : methods) {
        const std::string label = "{method=\"" + EscapePrometheusLabelValue(method.method) + "\"}";
        const double method_completed = std::max<uint64_t>(method.completed_requests, 1);
        out << "mrpc_method_requests_total" << label << ' ' << method.total_requests << '\n';
        out << "mrpc_method_requests_inflight" << label << ' ' << method.inflight_requests
            << '\n';
        out << "mrpc_method_requests_completed_total" << label << ' ' << method.completed_requests
            << '\n';
        out << "mrpc_method_responses_success_total" << label << ' ' << method.success_responses
            << '\n';
        out << "mrpc_method_responses_error_total" << label << ' ' << method.error_responses
            << '\n';
        out << "mrpc_method_responses_timeout_total" << label << ' ' << method.timeout_responses
            << '\n';
        out << "mrpc_method_responses_overload_total" << label << ' '
            << method.overload_responses << '\n';
        out << "mrpc_method_latency_queue_average_ms" << label << ' '
            << MicrosecondsToMilliseconds(method.queue_latency_us_total) / method_completed
            << '\n';
        out << "mrpc_method_latency_queue_max_ms" << label << ' '
            << MicrosecondsToMilliseconds(method.queue_latency_us_max) << '\n';
        out << "mrpc_method_latency_handler_average_ms" << label << ' '
            << MicrosecondsToMilliseconds(method.handler_latency_us_total) / method_completed
            << '\n';
        out << "mrpc_method_latency_handler_max_ms" << label << ' '
            << MicrosecondsToMilliseconds(method.handler_latency_us_max) << '\n';
        out << "mrpc_method_latency_io_return_average_ms" << label << ' '
            << MicrosecondsToMilliseconds(method.io_return_latency_us_total) / method_completed
            << '\n';
        out << "mrpc_method_latency_io_return_max_ms" << label << ' '
            << MicrosecondsToMilliseconds(method.io_return_latency_us_max) << '\n';
        out << "mrpc_method_latency_end_to_end_average_ms" << label << ' '
            << MicrosecondsToMilliseconds(method.end_to_end_latency_us_total) / method_completed
            << '\n';
        out << "mrpc_method_latency_end_to_end_max_ms" << label << ' '
            << MicrosecondsToMilliseconds(method.end_to_end_latency_us_max) << '\n';
    }
    return out.str();
}

Observability::Snapshot Observability::LoadSnapshot() const {
    Snapshot snapshot;
    snapshot.accepted_connections = accepted_connections_.load(std::memory_order_relaxed);
    snapshot.active_connections = active_connections_.load(std::memory_order_relaxed);
    snapshot.total_requests = total_requests_.load(std::memory_order_relaxed);
    snapshot.inflight_requests = inflight_requests_.load(std::memory_order_relaxed);
    snapshot.completed_requests = completed_requests_.load(std::memory_order_relaxed);
    snapshot.success_responses = success_responses_.load(std::memory_order_relaxed);
    snapshot.error_responses = error_responses_.load(std::memory_order_relaxed);
    snapshot.timeout_responses = timeout_responses_.load(std::memory_order_relaxed);
    snapshot.overload_responses = overload_responses_.load(std::memory_order_relaxed);
    snapshot.protocol_errors = protocol_errors_.load(std::memory_order_relaxed);
    snapshot.worker_rejections = worker_rejections_.load(std::memory_order_relaxed);
    snapshot.queue_latency_us_total = queue_latency_us_total_.load(std::memory_order_relaxed);
    snapshot.queue_latency_us_max = queue_latency_us_max_.load(std::memory_order_relaxed);
    snapshot.handler_latency_us_total = handler_latency_us_total_.load(std::memory_order_relaxed);
    snapshot.handler_latency_us_max = handler_latency_us_max_.load(std::memory_order_relaxed);
    snapshot.io_return_latency_us_total =
        io_return_latency_us_total_.load(std::memory_order_relaxed);
    snapshot.io_return_latency_us_max = io_return_latency_us_max_.load(std::memory_order_relaxed);
    snapshot.end_to_end_latency_us_total =
        end_to_end_latency_us_total_.load(std::memory_order_relaxed);
    snapshot.end_to_end_latency_us_max =
        end_to_end_latency_us_max_.load(std::memory_order_relaxed);
    return snapshot;
}

std::vector<Observability::MethodSnapshot> Observability::LoadMethodSnapshots() const {
    std::vector<MethodSnapshot> snapshots;
    std::lock_guard<std::mutex> lock(method_stats_mutex_);
    snapshots.reserve(method_stats_.size());
    for (const auto& [method, stats] : method_stats_) {
        snapshots.push_back(MethodSnapshot{
            method,
            stats.total_requests,
            stats.inflight_requests,
            stats.completed_requests,
            stats.success_responses,
            stats.error_responses,
            stats.timeout_responses,
            stats.overload_responses,
            stats.queue_latency_us_total,
            stats.queue_latency_us_max,
            stats.handler_latency_us_total,
            stats.handler_latency_us_max,
            stats.io_return_latency_us_total,
            stats.io_return_latency_us_max,
            stats.end_to_end_latency_us_total,
            stats.end_to_end_latency_us_max,
        });
    }
    std::sort(snapshots.begin(), snapshots.end(),
              [](const MethodSnapshot& left, const MethodSnapshot& right) {
                  return left.method < right.method;
              });
    return snapshots;
}

bool Observability::ShouldTrace(const RpcRequest& request, const RpcResponse& response,
                                std::chrono::milliseconds end_to_end_latency) const {
    if (!options_.enable_request_trace) {
        return false;
    }
    if (options_.trace_all_requests) {
        return true;
    }
    return !IsSuccess(response.status_code) ||
           end_to_end_latency >= options_.slow_request_threshold;
}

void Observability::LogTrace(const RpcRequest& request, const RpcResponse& response,
                             std::chrono::milliseconds queue_latency,
                             std::chrono::milliseconds handler_latency,
                             std::chrono::milliseconds io_return_latency,
                             std::chrono::milliseconds end_to_end_latency) const {
    const bool is_slow = end_to_end_latency >= options_.slow_request_threshold;
    std::lock_guard<std::mutex> lock(log_mutex_);
    std::cout << (is_slow ? "[slow] " : "[trace] ") << "request_id=" << request.request_id
              << " method=" << request.method << " client=" << request.client_label
              << " io_thread=" << request.io_thread_index
              << " connection_id=" << request.connection_id
              << " status=" << static_cast<uint16_t>(response.status_code)
              << " status_text=" << StatusCodeToString(response.status_code)
              << " timeout_ms=" << request.timeout_ms
              << " queue_ms=" << queue_latency.count()
              << " handler_ms=" << handler_latency.count()
              << " io_return_ms=" << io_return_latency.count()
              << " end_to_end_ms=" << end_to_end_latency.count()
              << " request_bytes=" << request.payload.size()
              << " response_bytes=" << response.payload.size() << '\n';
}
