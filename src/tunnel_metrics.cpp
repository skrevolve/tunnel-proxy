#include "core/tunnel_metrics.h"
#include "utils/logger.h"

#include <string>

namespace proxy {

// ── 기록 API ────────────────────────────────────────────────────────────────

void TunnelMetrics::record_connection_attempt() {
    connection_attempts_.fetch_add(1, std::memory_order_relaxed);
}

void TunnelMetrics::record_connection_success() {
    connection_successes_.fetch_add(1, std::memory_order_relaxed);
}

void TunnelMetrics::record_connection_failure() {
    connection_failures_.fetch_add(1, std::memory_order_relaxed);
}

void TunnelMetrics::record_reconnect() {
    reconnects_.fetch_add(1, std::memory_order_relaxed);
}

void TunnelMetrics::record_bytes_sent(size_t bytes) {
    bytes_sent_.fetch_add(bytes, std::memory_order_relaxed);
}

void TunnelMetrics::record_bytes_received(size_t bytes) {
    bytes_received_.fetch_add(bytes, std::memory_order_relaxed);
}

void TunnelMetrics::record_error() {
    errors_.fetch_add(1, std::memory_order_relaxed);
}

// ── 조회 API ────────────────────────────────────────────────────────────────

uint64_t TunnelMetrics::total_connection_attempts()  const {
    return connection_attempts_.load(std::memory_order_relaxed);
}

uint64_t TunnelMetrics::total_connection_successes() const {
    return connection_successes_.load(std::memory_order_relaxed);
}

uint64_t TunnelMetrics::total_connection_failures()  const {
    return connection_failures_.load(std::memory_order_relaxed);
}

uint64_t TunnelMetrics::total_reconnects()           const {
    return reconnects_.load(std::memory_order_relaxed);
}

uint64_t TunnelMetrics::total_bytes_sent()           const {
    return bytes_sent_.load(std::memory_order_relaxed);
}

uint64_t TunnelMetrics::total_bytes_received()       const {
    return bytes_received_.load(std::memory_order_relaxed);
}

uint64_t TunnelMetrics::total_errors()               const {
    return errors_.load(std::memory_order_relaxed);
}

double TunnelMetrics::error_rate() const {
    uint64_t attempts = total_connection_attempts();
    if (attempts == 0) return 0.0;
    return static_cast<double>(total_errors()) /
           static_cast<double>(attempts);
}

// ── reset / log ──────────────────────────────────────────────────────────────

void TunnelMetrics::reset() {
    connection_attempts_.store(0, std::memory_order_relaxed);
    connection_successes_.store(0, std::memory_order_relaxed);
    connection_failures_.store(0, std::memory_order_relaxed);
    reconnects_.store(0, std::memory_order_relaxed);
    bytes_sent_.store(0, std::memory_order_relaxed);
    bytes_received_.store(0, std::memory_order_relaxed);
    errors_.store(0, std::memory_order_relaxed);
}

void TunnelMetrics::log_summary(const std::string& prefix) const {
    const std::string tag = "[" + (prefix.empty() ? "metrics" : prefix) + "]";

    Logger::info(tag +
                 " connections: attempts="  + std::to_string(total_connection_attempts())  +
                 " success="                + std::to_string(total_connection_successes()) +
                 " failure="                + std::to_string(total_connection_failures())  +
                 " reconnects="             + std::to_string(total_reconnects()));

    Logger::info(tag +
                 " transfer: sent="         + std::to_string(total_bytes_sent())     + "B" +
                 " received="               + std::to_string(total_bytes_received()) + "B");

    Logger::info(tag +
                 " errors="                 + std::to_string(total_errors()) +
                 " error_rate="             + std::to_string(error_rate()));
}

} // namespace proxy
