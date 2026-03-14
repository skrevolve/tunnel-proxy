#pragma once

#include "core/tunnel_protocol.h"

#include <string>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <thread>
#include <future>
#include <optional>
#include <vector>

namespace proxy {

/**
 * @file tunnel_server.h
 * @brief Phase 6-C/D — 터널 서버 (에이전트 연결 수신 + 외부 클라이언트 포워딩)
 *
 * ── 완성된 전체 데이터 흐름 (Phase 6-D) ──────────────────────────────────────
 *
 *   외부 클라이언트          TunnelServer              TunnelAgent (NAT 뒤)
 *       │                  [proxy_port]  [agent_port]        │
 *       │──── TCP ─────▶  accept()                          │
 *       │              open_session()                        │
 *       │              OPEN(session_id, target) ───────────▶ │
 *       │                              OPEN_ACK ◀─────────   │──▶ 내부 서버
 *       │              set_session_external_fd()             │
 *       │◀──── DATA ──── external_fd ◀── DATA frame ◀────    │
 *       │───── DATA ───▶ DATA frame ─────────────────────▶  │
 *       │              CLOSE ──────────────────────────────▶ │
 *
 * ── 두 개의 리스닝 포트 ───────────────────────────────────────────────────────
 *
 *   agent_port_ (기본 9900):
 *     에이전트가 역방향 TCP 연결을 맺는 포트.
 *     run() 메인 스레드가 accept 루프 담당.
 *
 *   proxy_port_ (기본 0 = 비활성):
 *     외부 클라이언트가 연결하는 포트.
 *     proxy_listener_thread_가 별도로 accept 루프 담당.
 *
 * ── OPEN_ACK 대기 메커니즘 ────────────────────────────────────────────────────
 *
 *   외부 클라이언트 연결 시:
 *     1. open_session() → OPEN 프레임 에이전트 전송 + pending_open_에 promise 등록
 *     2. future.wait_for(10s): 에이전트의 OPEN_ACK 대기
 *     3. handle_agent_frame(OPEN_ACK) → promise.set_value() → future 완료
 *     4. set_session_external_fd() → 이후 DATA 포워딩 활성화
 *
 *   왜 future/promise인가:
 *     OPEN을 보내는 스레드(external client thread)와
 *     OPEN_ACK를 받는 스레드(per-agent thread)가 다르다.
 *     condition_variable보다 future가 단순하고 타임아웃 처리가 직관적.
 *
 * ── 동시성 모델 ───────────────────────────────────────────────────────────────
 *
 *   - main thread (run()):              agent accept 루프
 *   - proxy_listener_thread_:           external client accept 루프
 *   - per-agent thread (detach):        HELLO 교환 + 프레임 수신 루프
 *   - per-external-client thread (det): open_session → OPEN_ACK wait → 포워딩 루프
 *
 *   agents_mutex_:        agents_ 맵 보호
 *   sessions_mutex_:      sessions_ 맵 보호
 *   pending_open_mutex_:  pending_open_ 맵 보호
 *   forward_target_mutex_: forward_target_ 읽기/쓰기 보호
 *   AgentConn::send_mutex_: 에이전트별 fd 쓰기 직렬화
 */
class TunnelServer {
public:
    /**
     * @param agent_port      에이전트 역방향 연결 수신 포트 (기본값 9900)
     * @param proxy_port      외부 클라이언트 연결 수신 포트 (0이면 비활성)
     * @param agent_timeout_s 에이전트 heartbeat 타임아웃 (초).
     *                        이 시간 동안 HEARTBEAT가 오지 않으면 연결 종료.
     *                        0이면 watchdog 비활성.
     */
    explicit TunnelServer(int agent_port = 9900, int proxy_port = 0,
                          int agent_timeout_s = 90);
    ~TunnelServer();

    /**
     * 에이전트 수신 루프 시작 (블로킹)
     *
     * proxy_port > 0이면 proxy_listener_thread_도 함께 시작.
     */
    void run();

    /** accept 루프 중지 + 모든 연결 종료 */
    void stop();

    /**
     * 외부 클라이언트 → 터널 포워딩 라우팅 설정
     *
     * proxy_port_로 들어오는 외부 클라이언트를 지정 에이전트의 내부 서버로 라우팅.
     * 동일 에이전트로 여러 세션이 동시에 존재할 수 있다 (session_id로 구분).
     *
     * @param agent_id    라우팅할 에이전트 ID
     * @param target_ip   에이전트가 연결할 내부 서버 IP
     * @param target_port 에이전트가 연결할 내부 서버 포트
     */
    void set_forward_target(const std::string& agent_id,
                            const std::string& target_ip, uint16_t target_port);

    // ── 세션 API (내부 + 6-D 공개 인터페이스) ────────────────────────────────

    /**
     * 세션 개설: 지정 에이전트에 OPEN 프레임 전송 + session_id 반환
     *
     * @return 발급된 session_id (에이전트 없음 또는 전송 실패 시 0)
     */
    uint32_t open_session(const std::string& agent_id,
                          const std::string& target_ip, uint16_t target_port);

    /**
     * 세션에 외부 클라이언트 fd 연결 (OPEN_ACK 수신 후 호출)
     *
     * @return true = 세션 찾음, false = 없음
     */
    bool set_session_external_fd(uint32_t session_id, int external_fd);

    // ── 상태 조회 ─────────────────────────────────────────────────────────────

    uint32_t get_agent_count() const;
    uint32_t get_session_count() const;
    std::vector<std::string> get_agent_ids() const;

private:
    // ── 내부 자료구조 ─────────────────────────────────────────────────────────

    struct AgentConn {
        int         fd;
        std::string agent_id;
        std::mutex  send_mutex;

        /**
         * 마지막 HEARTBEAT 수신 시각 (nanoseconds since steady_clock epoch)
         *
         * 에이전트 등록 시 현재 시각으로 초기화.
         * HEARTBEAT 프레임 수신 시 갱신.
         * watchdog_loop()에서 타임아웃 계산에 사용.
         */
        std::atomic<int64_t> last_heartbeat_ns;

        AgentConn(int f, std::string id);
        ~AgentConn();
        AgentConn(const AgentConn&)            = delete;
        AgentConn& operator=(const AgentConn&) = delete;
    };

    struct Session {
        uint32_t    session_id;
        std::string agent_id;
        int         external_fd{-1};
    };

    /**
     * 외부 클라이언트 → 에이전트 라우팅 설정
     * set_forward_target()으로 설정. nullopt이면 외부 연결 거부.
     */
    struct ForwardTarget {
        std::string agent_id;
        std::string target_ip;
        uint16_t    target_port;
    };

    // ── 내부 함수 ─────────────────────────────────────────────────────────────

    int create_listen_socket(int port);

    // 에이전트 연결: HELLO 교환 → 등록 → 프레임 루프 (detached thread)
    void handle_agent_connection(int agent_fd);
    void handle_agent_frame(const std::string& agent_id,
                            const TunnelFrame& frame);
    void unregister_agent(const std::string& agent_id);

    /**
     * 외부 클라이언트 연결 처리 (detached thread에서 실행)
     *
     * 1. ForwardTarget 조회
     * 2. open_session() + pending_open_에 promise 등록
     * 3. future.wait_for(OPEN_ACK_TIMEOUT): 에이전트의 내부 연결 대기
     * 4. set_session_external_fd() → DATA 포워딩 활성화
     * 5. 외부 클라이언트 read 루프: DATA 프레임 → 에이전트 전송
     * 6. 연결 종료 시 CLOSE 프레임 에이전트 전송 + close_session()
     */
    void handle_external_connection(int external_fd);

    /**
     * external client accept 루프 (proxy_listener_thread_에서 실행)
     *
     * proxy_listen_fd_에서 accept → handle_external_connection() detach.
     */
    void proxy_accept_loop();

    /**
     * 에이전트 heartbeat 감시 스레드 함수
     *
     * 10초 간격으로 모든 에이전트의 last_heartbeat_ns를 확인.
     * agent_timeout_s_ 초 이상 HEARTBEAT 미수신 시:
     *   - 에이전트 fd shutdown → per-agent thread의 recv_frame 탈출 → unregister_agent 호출
     *
     * agent_timeout_s_ == 0이면 watchdog 비활성 (run()에서 스레드 미시작).
     */
    void watchdog_loop();

    void close_session(uint32_t session_id);
    uint32_t generate_session_id();

    bool recv_exact(int fd, uint8_t* buf, size_t n);
    bool recv_frame(int fd, TunnelFrame& out);
    void send_to_agent(const std::string& agent_id, const TunnelFrame& frame);

    // ── 멤버 변수 ─────────────────────────────────────────────────────────────

    int agent_port_;
    int proxy_port_;
    int agent_timeout_s_;

    int agent_listen_fd_{-1};
    int proxy_listen_fd_{-1};

    std::atomic<bool>     running_{false};
    std::atomic<uint32_t> next_session_id_{1};

    std::thread proxy_listener_thread_;
    std::thread watchdog_thread_;

    mutable std::mutex agents_mutex_;
    std::unordered_map<std::string, std::shared_ptr<AgentConn>> agents_;

    mutable std::mutex sessions_mutex_;
    std::unordered_map<uint32_t, Session> sessions_;

    /**
     * OPEN_ACK 대기 맵: session_id → promise<void>
     *
     * handle_external_connection()이 promise를 등록하고 future.wait_for()로 대기.
     * handle_agent_frame(OPEN_ACK)이 promise.set_value()로 시그널.
     */
    std::mutex pending_open_mutex_;
    std::unordered_map<uint32_t, std::promise<void>> pending_open_;

    mutable std::mutex forward_target_mutex_;
    std::optional<ForwardTarget> forward_target_;

    /// OPEN_ACK 대기 최대 시간 (초)
    static constexpr int OPEN_ACK_TIMEOUT_S = 10;
};

} // namespace proxy
