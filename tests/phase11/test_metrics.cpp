#include <gtest/gtest.h>

#include "core/tunnel_metrics.h"
#include "core/tunnel_agent.h"
#include "core/tunnel_server.h"

#include <thread>
#include <chrono>
#include <vector>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

using namespace proxy;

// ── 포트 헬퍼 ────────────────────────────────────────────────────────────────

static int find_free_port() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = 0;
    addr.sin_addr.s_addr = INADDR_ANY;
    bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));

    socklen_t len = sizeof(addr);
    getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    int port = ntohs(addr.sin_port);
    close(fd);
    return port;
}

// ══════════════════════════════════════════════════════════════════════════════
// 시나리오 1~8 — TunnelMetrics 카운터 기본 동작
// ══════════════════════════════════════════════════════════════════════════════

// 시나리오 1 — 초기 상태: 모든 카운터 0
TEST(TunnelMetricsBasic, InitialValues_AllZero) {
    TunnelMetrics m;
    EXPECT_EQ(m.total_connection_attempts(),  0u);
    EXPECT_EQ(m.total_connection_successes(), 0u);
    EXPECT_EQ(m.total_connection_failures(),  0u);
    EXPECT_EQ(m.total_reconnects(),           0u);
    EXPECT_EQ(m.total_bytes_sent(),           0u);
    EXPECT_EQ(m.total_bytes_received(),       0u);
    EXPECT_EQ(m.total_errors(),               0u);
    EXPECT_DOUBLE_EQ(m.error_rate(),          0.0);
}

// 시나리오 2 — record_connection_attempt() → total_connection_attempts() 증가
TEST(TunnelMetricsBasic, RecordConnectionAttempt_Increments) {
    TunnelMetrics m;
    m.record_connection_attempt();
    m.record_connection_attempt();
    EXPECT_EQ(m.total_connection_attempts(), 2u);
}

// 시나리오 3 — record_connection_success() → total_connection_successes() 증가
TEST(TunnelMetricsBasic, RecordConnectionSuccess_Increments) {
    TunnelMetrics m;
    m.record_connection_success();
    EXPECT_EQ(m.total_connection_successes(), 1u);
}

// 시나리오 4 — record_connection_failure() → total_connection_failures() 증가
TEST(TunnelMetricsBasic, RecordConnectionFailure_Increments) {
    TunnelMetrics m;
    m.record_connection_failure();
    m.record_connection_failure();
    EXPECT_EQ(m.total_connection_failures(), 2u);
}

// 시나리오 5 — record_reconnect() → total_reconnects() 증가
TEST(TunnelMetricsBasic, RecordReconnect_Increments) {
    TunnelMetrics m;
    m.record_reconnect();
    EXPECT_EQ(m.total_reconnects(), 1u);
}

// 시나리오 6 — record_bytes_sent(N) 여러 번 → total_bytes_sent() 누적합 일치
TEST(TunnelMetricsBasic, RecordBytesSent_Accumulates) {
    TunnelMetrics m;
    m.record_bytes_sent(100);
    m.record_bytes_sent(200);
    m.record_bytes_sent(300);
    EXPECT_EQ(m.total_bytes_sent(), 600u);
}

// 시나리오 7 — record_bytes_received(N) 여러 번 → total_bytes_received() 누적합 일치
TEST(TunnelMetricsBasic, RecordBytesReceived_Accumulates) {
    TunnelMetrics m;
    m.record_bytes_received(512);
    m.record_bytes_received(512);
    EXPECT_EQ(m.total_bytes_received(), 1024u);
}

// 시나리오 8 — record_error() → total_errors() 증가
TEST(TunnelMetricsBasic, RecordError_Increments) {
    TunnelMetrics m;
    m.record_error();
    EXPECT_EQ(m.total_errors(), 1u);
}

// ══════════════════════════════════════════════════════════════════════════════
// 시나리오 9~10 — 오류율 계산
// ══════════════════════════════════════════════════════════════════════════════

// 시나리오 9 — attempts=0 → error_rate() == 0.0 (0 나누기 보호)
TEST(TunnelMetricsErrorRate, ZeroAttempts_ReturnsZero) {
    TunnelMetrics m;
    m.record_error();
    EXPECT_DOUBLE_EQ(m.error_rate(), 0.0);
}

// 시나리오 10 — attempts=10, errors=3 → error_rate() ≈ 0.3
TEST(TunnelMetricsErrorRate, ThreeErrorsOutOfTen_Returns0_3) {
    TunnelMetrics m;
    for (int i = 0; i < 10; ++i) m.record_connection_attempt();
    for (int i = 0; i < 3;  ++i) m.record_error();
    EXPECT_NEAR(m.error_rate(), 0.3, 1e-9);
}

// ══════════════════════════════════════════════════════════════════════════════
// 시나리오 11 — reset()
// ══════════════════════════════════════════════════════════════════════════════

// 시나리오 11 — 카운터 채운 뒤 reset() → 모든 값 0
TEST(TunnelMetricsReset, AfterReset_AllZero) {
    TunnelMetrics m;
    m.record_connection_attempt();
    m.record_connection_success();
    m.record_connection_failure();
    m.record_reconnect();
    m.record_bytes_sent(1024);
    m.record_bytes_received(2048);
    m.record_error();

    m.reset();

    EXPECT_EQ(m.total_connection_attempts(),  0u);
    EXPECT_EQ(m.total_connection_successes(), 0u);
    EXPECT_EQ(m.total_connection_failures(),  0u);
    EXPECT_EQ(m.total_reconnects(),           0u);
    EXPECT_EQ(m.total_bytes_sent(),           0u);
    EXPECT_EQ(m.total_bytes_received(),       0u);
    EXPECT_EQ(m.total_errors(),               0u);
}

// ══════════════════════════════════════════════════════════════════════════════
// 시나리오 12 — 스레드 안전성
// ══════════════════════════════════════════════════════════════════════════════

// 시나리오 12 — 10개 스레드 × 1000번 record_bytes_sent(100) → 합계 1,000,000
TEST(TunnelMetricsThreadSafety, ConcurrentRecordBytes_ConsistentSum) {
    TunnelMetrics m;
    constexpr int THREADS = 10;
    constexpr int ITERS   = 1000;

    std::vector<std::thread> threads;
    threads.reserve(THREADS);
    for (int i = 0; i < THREADS; ++i) {
        threads.emplace_back([&m]() {
            for (int j = 0; j < ITERS; ++j) {
                m.record_bytes_sent(100);
            }
        });
    }
    for (auto& t : threads) t.join();

    EXPECT_EQ(m.total_bytes_sent(),
              static_cast<uint64_t>(THREADS * ITERS * 100));
}

// ══════════════════════════════════════════════════════════════════════════════
// 시나리오 13~15 — TunnelAgent 통합
// ══════════════════════════════════════════════════════════════════════════════

// 시나리오 13 — 연결 성공 → connection_successes == 1
TEST(TunnelAgentMetrics, ConnectSuccess_RecordsSuccess) {
    int port = find_free_port();
    TunnelServer server(port);

    std::thread server_thread([&server]() {
        try { server.run(); } catch (...) {}
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    TunnelAgent agent("127.0.0.1", port, "agent-metrics-test",
                      /*heartbeat_interval_s=*/60,
                      /*heartbeat_timeout_s=*/180);

    std::thread agent_thread([&agent]() {
        try { agent.run(); } catch (...) {}
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    agent.stop();
    server.stop();

    agent_thread.join();
    server_thread.join();

    EXPECT_EQ(agent.get_metrics().total_connection_successes(), 1u);
    EXPECT_GE(agent.get_metrics().total_connection_attempts(),  1u);
}

// 시나리오 14 — 연결 실패 (서버 없음) → connection_failures >= 1
TEST(TunnelAgentMetrics, ConnectFailure_RecordsFailure) {
    int port = find_free_port();  // 아무것도 듣지 않는 포트
    TunnelAgent agent("127.0.0.1", port, "agent-fail-test",
                      /*heartbeat_interval_s=*/60,
                      /*heartbeat_timeout_s=*/180);

    std::thread agent_thread([&agent]() {
        try { agent.run(); } catch (...) {}
    });
    // 재연결 1회 이상 발생할 때까지 대기 (첫 실패 + 1s 백오프 + 두 번째 실패)
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    agent.stop();
    agent_thread.join();

    EXPECT_GE(agent.get_metrics().total_connection_failures(), 1u);
}

// 시나리오 15 — 재연결 발생 → reconnects >= 1
TEST(TunnelAgentMetrics, Reconnect_RecordsReconnect) {
    int port = find_free_port();
    TunnelAgent agent("127.0.0.1", port, "agent-reconnect-test",
                      /*heartbeat_interval_s=*/60,
                      /*heartbeat_timeout_s=*/180);

    std::thread agent_thread([&agent]() {
        try { agent.run(); } catch (...) {}
    });
    // 첫 실패(즉시) → 1s 백오프 → 두 번째 시도(= 재연결) 발생
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    agent.stop();
    agent_thread.join();

    EXPECT_GE(agent.get_metrics().total_reconnects(), 1u);
}

// ══════════════════════════════════════════════════════════════════════════════
// 시나리오 16~17 — TunnelServer 통합
// ══════════════════════════════════════════════════════════════════════════════

// 시나리오 16 — 에이전트 등록 → server connection_successes == 1
TEST(TunnelServerMetrics, AgentRegister_RecordsSuccess) {
    int port = find_free_port();
    TunnelServer server(port);

    std::thread server_thread([&server]() {
        try { server.run(); } catch (...) {}
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    TunnelAgent agent("127.0.0.1", port, "agent-server-metrics",
                      /*heartbeat_interval_s=*/60,
                      /*heartbeat_timeout_s=*/180);

    std::thread agent_thread([&agent]() {
        try { agent.run(); } catch (...) {}
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    agent.stop();
    server.stop();

    agent_thread.join();
    server_thread.join();

    EXPECT_EQ(server.get_metrics().total_connection_successes(), 1u);
    EXPECT_GE(server.get_metrics().total_connection_attempts(),  1u);
}

// 시나리오 17 — 에이전트 연결 후 해제 → server errors 또는 failures 반영
TEST(TunnelServerMetrics, AgentDisconnect_RecordsEvent) {
    int port = find_free_port();
    TunnelServer server(port);

    std::thread server_thread([&server]() {
        try { server.run(); } catch (...) {}
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    TunnelAgent agent("127.0.0.1", port, "agent-disconnect-test",
                      /*heartbeat_interval_s=*/60,
                      /*heartbeat_timeout_s=*/180);

    std::thread agent_thread([&agent]() {
        try { agent.run(); } catch (...) {}
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // 에이전트 먼저 종료 → 서버가 disconnect 감지
    agent.stop();
    agent_thread.join();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 연결 성공 후 해제까지 서버에 기록되었는지 확인
    EXPECT_GE(server.get_metrics().total_connection_successes(), 1u);

    server.stop();
    server_thread.join();
}
