#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace proxy {

/**
 * @file tunnel_metrics.h
 * @brief Phase 11-C — 터널 메트릭 수집
 *
 * TunnelAgent / TunnelServer에 삽입되어 런타임 통계를 수집한다.
 * 모든 카운터는 atomic<uint64_t>로 thread-safe.
 * log_summary()로 현재 상태를 Logger에 출력.
 *
 * ── 수집 항목 ────────────────────────────────────────────────────────────────
 *
 *   connection_attempts  : 연결 시도 횟수 (재연결 포함)
 *   connection_successes : HELLO_ACK까지 완료한 횟수
 *   connection_failures  : 연결 실패 횟수 (connect 오류 / HELLO 실패)
 *   reconnects           : 재연결 시도 횟수 (첫 연결 제외)
 *   bytes_sent           : 서버/에이전트로 전송한 누적 바이트
 *   bytes_received       : 서버/에이전트에서 수신한 누적 바이트
 *   errors               : 전송 실패 등 런타임 오류 횟수
 *
 * ── error_rate() 정의 ─────────────────────────────────────────────────────
 *
 *   errors / connection_attempts.
 *   connection_attempts == 0이면 0.0 반환 (0 나누기 보호).
 */
class TunnelMetrics {
public:
    // ── 기록 API ──────────────────────────────────────────────────────────────

    void record_connection_attempt();
    void record_connection_success();
    void record_connection_failure();
    void record_reconnect();
    void record_bytes_sent(size_t bytes);
    void record_bytes_received(size_t bytes);
    void record_error();

    // ── 조회 API ──────────────────────────────────────────────────────────────

    uint64_t total_connection_attempts()  const;
    uint64_t total_connection_successes() const;
    uint64_t total_connection_failures()  const;
    uint64_t total_reconnects()           const;
    uint64_t total_bytes_sent()           const;
    uint64_t total_bytes_received()       const;
    uint64_t total_errors()               const;

    /**
     * 오류율: total_errors / total_connection_attempts
     * total_connection_attempts == 0이면 0.0 반환.
     */
    double error_rate() const;

    /** 모든 카운터를 0으로 초기화 */
    void reset();

    /**
     * 현재 메트릭을 Logger::info로 출력
     * @param prefix 로그 태그 접두사 (예: "agent", "server"). 비어있으면 "metrics".
     */
    void log_summary(const std::string& prefix = "") const;

private:
    std::atomic<uint64_t> connection_attempts_{0};
    std::atomic<uint64_t> connection_successes_{0};
    std::atomic<uint64_t> connection_failures_{0};
    std::atomic<uint64_t> reconnects_{0};
    std::atomic<uint64_t> bytes_sent_{0};
    std::atomic<uint64_t> bytes_received_{0};
    std::atomic<uint64_t> errors_{0};
};

} // namespace proxy
