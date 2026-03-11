#pragma once

#include "core/tunnel_protocol.h"

#include <string>
#include <atomic>
#include <cstdint>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <vector>

namespace proxy {

/**
 * @file tunnel_server.h
 * @brief Phase 6-C — 터널 서버 (에이전트 연결 수신측)
 *
 * ── 전체 아키텍처에서 TunnelServer의 역할 ──────────────────────────────────────
 *
 *   외부 클라이언트          서버                에이전트 (NAT 뒤)
 *       │                  TunnelServer              TunnelAgent
 *       │──── TCP ─────▶  [proxy_port]               │
 *       │                      │                     │
 *       │              session_id 발급                │
 *       │              OPEN ─────────────────────▶  │
 *       │              OPEN_ACK ◀──────────────────  │──▶ 내부 서버
 *       │◀──── DATA ──────────────────────────────  DATA
 *
 *   TunnelServer가 관리하는 것:
 *     - 에이전트 연결 목록 (agents_): HELLO로 등록, 연결 끊김 시 자동 해제
 *     - 세션 맵 (sessions_): session_id ↔ (agent, external_fd) 매핑
 *     - session_id 발급 (next_session_id_): 단조 증가, 0은 컨트롤 채널 예약
 *
 * ── 에이전트 연결 처리 흐름 ──────────────────────────────────────────────────
 *
 *   accept(agent_listen_fd_) → agent_fd
 *   → handle_agent_connection(agent_fd) [detached thread]
 *       recv HELLO → register in agents_ → send HELLO_ACK
 *       → frame loop:
 *           HEARTBEAT     → send HEARTBEAT_ACK
 *           DATA          → forward to session의 external_fd (6-D에서 활성화)
 *           CLOSE         → close_session()
 *           OPEN_ACK      → session 상태 갱신 (6-D에서 활용)
 *       → agent 연결 종료 → unregister_agent()
 *
 * ── 세션 개설 흐름 (6-D에서 외부 클라이언트 연결 시 호출) ─────────────────────
 *
 *   open_session(agent_id, target_ip, target_port)
 *     → generate_session_id() → session_id
 *     → sessions_에 Session 추가
 *     → send OPEN(session_id, target_ip, target_port) to agent
 *     → return session_id
 *
 *   외부 클라이언트 fd는 OPEN_ACK 수신 후 6-D에서 set_session_external_fd()로 설정.
 *
 * ── 동시성 모델 ───────────────────────────────────────────────────────────────
 *
 *   - main thread (run()):        accept 루프 (agent_listen_fd_)
 *   - per-agent thread (detach):  HELLO 교환 + 프레임 수신 루프
 *   - 6-D threads:                외부 클라이언트 연결 처리
 *
 *   agents_mutex_:   agents_ 맵 보호. 등록/해제 시 잠금.
 *   sessions_mutex_: sessions_ 맵 보호.
 *   AgentConn::send_mutex_: 에이전트별 fd 쓰기 직렬화.
 */
class TunnelServer {
public:
    /**
     * @param agent_port  에이전트가 역방향 연결하는 포트 (기본값 9900)
     */
    explicit TunnelServer(int agent_port = 9900);
    ~TunnelServer();

    /**
     * 에이전트 수신 루프 시작 (블로킹)
     *
     * agent_port_에 리스닝 소켓을 생성하고 accept 루프 진입.
     * 각 에이전트 연결을 detached thread로 처리한다.
     */
    void run();

    /** accept 루프 중지 + 모든 에이전트 연결 종료 */
    void stop();

    /**
     * 세션 개설: 지정 에이전트에 OPEN 프레임을 전송하고 session_id를 반환.
     *
     * Phase 6-D에서 외부 클라이언트 연결 시 호출.
     * OPEN_ACK 수신 후 set_session_external_fd()로 외부 클라이언트 fd를 연결.
     *
     * @param agent_id    OPEN을 보낼 에이전트 ID
     * @param target_ip   에이전트가 연결할 내부 서버 IP
     * @param target_port 에이전트가 연결할 내부 서버 포트
     * @return 발급된 session_id (에이전트를 찾을 수 없으면 0)
     */
    uint32_t open_session(const std::string& agent_id,
                          const std::string& target_ip, uint16_t target_port);

    /**
     * 세션에 외부 클라이언트 fd를 연결한다.
     *
     * Phase 6-D에서 OPEN_ACK 수신 후 호출.
     * 이후 에이전트의 DATA 프레임이 이 fd로 포워딩된다.
     *
     * @return true = 세션 찾음, false = 세션 없음
     */
    bool set_session_external_fd(uint32_t session_id, int external_fd);

    // ── 상태 조회 ─────────────────────────────────────────────────────────────

    /** 현재 연결된 에이전트 수 */
    uint32_t get_agent_count() const;

    /** 현재 활성 세션 수 */
    uint32_t get_session_count() const;

    /** 등록된 에이전트 ID 목록 */
    std::vector<std::string> get_agent_ids() const;

private:
    // ── 내부 자료구조 ─────────────────────────────────────────────────────────

    /**
     * 에이전트 연결 정보
     *
     * shared_ptr로 관리하는 이유:
     *   per-agent thread와 main thread(send_to_agent)가 동시에 접근.
     *   agents_ 맵에서 제거되어도 thread가 종료될 때까지 객체를 유지해야 함.
     *   unique_ptr은 이 공유 소유권을 표현 불가.
     */
    struct AgentConn {
        int         fd;
        std::string agent_id;
        std::mutex  send_mutex;  // 이 fd에 대한 쓰기 직렬화

        AgentConn(int f, std::string id)
            : fd(f), agent_id(std::move(id)) {}
        ~AgentConn();
        AgentConn(const AgentConn&)            = delete;
        AgentConn& operator=(const AgentConn&) = delete;
    };

    /**
     * 터널 세션 정보
     *
     * session_id:   TunnelFrame.session_id와 일치하는 고유 식별자
     * agent_id:     이 세션을 담당하는 에이전트
     * external_fd:  외부 클라이언트 소켓 fd. -1이면 아직 OPEN_ACK 미수신.
     *               Phase 6-D에서 set_session_external_fd()로 설정.
     */
    struct Session {
        uint32_t    session_id;
        std::string agent_id;
        int         external_fd{-1};
    };

    // ── 내부 함수 ─────────────────────────────────────────────────────────────

    /// 리스닝 소켓 생성 (SO_REUSEADDR + bind + listen)
    int create_listen_socket(int port);

    /**
     * 에이전트 연결 처리 (detached thread에서 실행)
     *
     * HELLO 수신 → agents_ 등록 → HELLO_ACK 송신 → 프레임 수신 루프.
     * 연결 종료 시 unregister_agent() 호출.
     */
    void handle_agent_connection(int agent_fd);

    /**
     * 에이전트에서 수신한 프레임 처리
     *
     * @param agent_id  프레임을 보낸 에이전트 ID
     * @param frame     수신된 프레임
     */
    void handle_agent_frame(const std::string& agent_id,
                            const TunnelFrame& frame);

    /**
     * 에이전트 연결 해제 + 해당 에이전트의 모든 세션 정리
     *
     * per-agent thread가 종료될 때 호출.
     * agents_ 맵에서 제거 → 세션들 CLOSE 처리.
     */
    void unregister_agent(const std::string& agent_id);

    /// session_id → Session 맵에서 제거
    void close_session(uint32_t session_id);

    /// 단조 증가하는 session_id 발급 (0은 컨트롤 채널로 예약되어 건너뜀)
    uint32_t generate_session_id();

    // ── 수신 유틸리티 ─────────────────────────────────────────────────────────

    bool recv_exact(int fd, uint8_t* buf, size_t n);
    bool recv_frame(int fd, TunnelFrame& out);

    /**
     * agent_id의 에이전트에 프레임 전송 (AgentConn::send_mutex_ 보호)
     *
     * agents_ 맵에서 shared_ptr을 꺼낸 뒤 락 해제.
     * shared_ptr가 살아있는 동안 AgentConn 객체는 유효.
     * 에이전트를 찾지 못하거나 전송 실패 시 조용히 반환.
     */
    void send_to_agent(const std::string& agent_id, const TunnelFrame& frame);

    // ── 멤버 변수 ─────────────────────────────────────────────────────────────

    int agent_port_;
    int agent_listen_fd_{-1};

    std::atomic<bool>     running_{false};
    std::atomic<uint32_t> next_session_id_{1};

    mutable std::mutex agents_mutex_;
    std::unordered_map<std::string, std::shared_ptr<AgentConn>> agents_;

    mutable std::mutex sessions_mutex_;
    std::unordered_map<uint32_t, Session> sessions_;
};

} // namespace proxy
