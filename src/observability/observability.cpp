#include "observability/observability.h"

#include <algorithm>
#include <iostream>
#include <sstream>

namespace {

using Clock = std::chrono::steady_clock;

int64_t ToMilliseconds(Clock::duration duration) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
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

void Observability::OnRequestDecoded() {
    total_requests_.fetch_add(1, std::memory_order_relaxed);
    inflight_requests_.fetch_add(1, std::memory_order_relaxed);
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
    return snapshot;
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
