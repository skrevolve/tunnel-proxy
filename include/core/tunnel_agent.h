#pragma once

#include "core/tunnel_protocol.h"
#include "core/tunnel_metrics.h"

#include <string>
#include <atomic>
#include <cstdint>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <thread>

namespace proxy {

/**
 * @file tunnel_agent.h
 * @brief Phase 6-B — 리버스 터널 에이전트
 *
 * ── 역방향 연결 구조 ──────────────────────────────────────────────────────────
 *
 *   일반 프록시:   클라이언트 → 서버 (클라이언트가 먼저 연결)
 *   리버스 터널:   에이전트 → 서버 (NAT 뒤의 에이전트가 먼저 서버에 연결)
 *
 *   서버는 에이전트의 연결을 유지해두고, 외부 요청이 오면
 *   이 기존 연결을 통해 에이전트에게 OPEN 프레임을 전송해 새 세션을 요청한다.
 *
 * ── 에이전트 동작 흐름 ────────────────────────────────────────────────────────
 *
 *   1. connect(server_ip, server_port) → server_fd 수립
 *   2. HELLO(agent_id) 송신
 *   3. HELLO_ACK 수신 → 등록 완료
 *   4. HEARTBEAT 스레드 시작 (heartbeat_interval_s_ 주기)
 *   5. server_fd에서 프레임 수신 루프:
 *      - OPEN          → 내부 타겟 연결 + 세션 생성 + per-session reader 시작
 *      - DATA          → 해당 세션의 target_fd로 데이터 전달
 *      - CLOSE         → 세션 target_fd 닫기
 *      - HEARTBEAT_ACK → 무시 (Phase 11-A에서 타임아웃 감지 추가 예정)
 *   6. per-session reader 스레드: target_fd에서 읽어 DATA 프레임으로 server에 전송
 *      target EOF 시 CLOSE 프레임 송신 후 세션 자체 정리 + 종료
 *
 * ── 세션 멀티플렉싱 ───────────────────────────────────────────────────────────
 *
 *   하나의 server_fd(TCP 연결) 위에 여러 세션을 동시에 유지.
 *   수신 측(main thread): session_id로 어느 target_fd에 데이터를 보낼지 분기.
 *   송신 측(per-session thread): target에서 읽은 데이터를 session_id가 붙은
 *                                DATA 프레임으로 포장해 server_fd에 전송.
 *
 * ── 동시성 모델 ───────────────────────────────────────────────────────────────
 *
 *   스레드 종류:
 *     - main thread (run()):     server_fd에서 프레임 읽기 (단독 reader)
 *     - heartbeat thread:        server_fd에 HEARTBEAT 쓰기
 *     - per-session thread (N):  target_fd에서 읽어 server_fd에 DATA 쓰기
 *
 *   동기화:
 *     send_mutex_:     server_fd 쓰기를 직렬화. heartbeat + per-session 스레드가 공유.
 *     sessions_mutex_: sessions_ 맵 접근 보호.
 *
 *   server_fd 읽기는 main thread 단독이므로 뮤텍스 불필요.
 */
class TunnelAgent {
public:
    /**
     * @param server_ip            서버 IPv4 주소
     * @param server_port          서버 포트 번호
     * @param agent_id             에이전트 고유 식별자 (HELLO 프레임에 포함)
     * @param heartbeat_interval_s HEARTBEAT 송신 주기 (초). 기본값 30초.
     * @param heartbeat_timeout_s  HEARTBEAT_ACK 무응답 타임아웃 (초).
     *                             0이면 interval * 3 자동 설정.
     */
    TunnelAgent(const std::string& server_ip, int server_port,
                const std::string& agent_id,
                int heartbeat_interval_s = 30,
                int heartbeat_timeout_s  = 0);

    ~TunnelAgent();

    /**
     * 서버 연결 → HELLO 교환 → 이벤트 루프 진입 (블로킹)
     *
     * 연결 실패 또는 단절 시 지수 백오프로 자동 재연결.
     *   1s → 2s → 4s → 8s → ... → MAX_RECONNECT_DELAY_S(60s) 반복.
     * stop() 호출 시에만 루프 탈출.
     */
    void run();

    /**
     * 이벤트 루프 중지
     *
     * server_fd shutdown → main thread의 recv_frame 탈출.
     * 모든 세션 target_fd close → per-session 스레드 종료 유도.
     * heartbeat 스레드 join.
     */
    void stop();

    /// 현재 활성 세션 수 반환
    uint32_t get_active_sessions() const;

    /**
     * 마지막 HEARTBEAT_ACK 이후 경과 시간 (초) 반환
     *
     * run() 호출 전에는 0 반환.
     * 타임아웃 감지 여부를 외부에서 확인할 때 사용.
     */
    int64_t seconds_since_last_ack() const;

    /**
     * 현재 재연결 대기 중인 지수 백오프 딜레이 (초) 반환
     *
     * 연결 중이거나 run() 미호출 시 0.
     * 재연결 대기 중에는 1~MAX_RECONNECT_DELAY_S 값.
     */
    int current_reconnect_delay() const;

    /** 수집된 메트릭 반환 */
    const TunnelMetrics& get_metrics() const { return metrics_; }

private:
    /**
     * 활성 세션 정보
     *
     * target_fd: 에이전트가 내부 서버에 수립한 TCP 소켓 fd
     * reader:    target_fd → server_fd 포워딩 스레드 (joinable)
     *
     * 스레드 종료 조건:
     *   A) target EOF/오류 → 스레드 스스로 CLOSE 송신 후 세션 맵에서 제거
     *   B) close_session() 호출 → target_fd shutdown → 스레드 recv 에러 → 종료 후 join
     */
    struct Session {
        int         target_fd;
        std::thread reader;

        explicit Session(int tfd) : target_fd(tfd) {}
        ~Session();
        Session(const Session&)            = delete;
        Session& operator=(const Session&) = delete;
    };

    // ── 네트워크 ──────────────────────────────────────────────────────────────

    /// 서버에 TCP 연결. 성공 시 fd 반환, 실패 시 예외.
    int connect_to_server();

    /// target_ip:target_port에 TCP 연결. 성공 시 fd 반환, 실패 시 예외.
    int connect_to_target(const std::string& ip, uint16_t port);

    /**
     * fd에서 정확히 n 바이트 읽기 (부분 읽기 반복 처리)
     *
     * TCP recv()는 요청보다 적게 반환할 수 있다.
     * n 바이트를 모두 읽을 때까지 반복한다.
     *
     * @return true = 성공, false = EOF 또는 오류
     */
    bool recv_exact(int fd, uint8_t* buf, size_t n);

    /**
     * fd에서 완전한 TunnelFrame 수신 (2단계: 헤더 16B → payload)
     *
     * @return true = 성공, false = 연결 종료 또는 파싱 오류
     */
    bool recv_frame(int fd, TunnelFrame& out);

    /**
     * server_fd에 프레임 직렬화 후 전송 (send_mutex_ 보호)
     *
     * MSG_NOSIGNAL: peer 닫힘 시 SIGPIPE 대신 errno=EPIPE 반환.
     * 전송 실패 시 예외를 던진다.
     */
    void send_frame(const TunnelFrame& frame);

    // ── 프레임 핸들러 ─────────────────────────────────────────────────────────

    /**
     * 단일 연결 수명 처리 (connect → HELLO → 이벤트 루프 → 종료)
     *
     * run()의 재연결 루프 안에서 호출.
     * 연결 실패 시 std::exception 던짐 → run()이 백오프 후 재시도.
     * 연결 종료(정상/비정상) 시 cleanup_connection() 호출 후 반환.
     * running_ 값을 변경하지 않는다.
     */
    void connect_and_run();

    /**
     * 현재 연결 자원 정리 (running_ 변경 없음)
     *
     * - sessions_: 모든 세션 target_fd 닫기, reader 스레드 detach
     * - heartbeat_thread_: join
     * - server_fd_: close
     *
     * stop()과 connect_and_run() 양쪽에서 호출.
     * 멱등: 이미 닫힌 fd는 건너뜀.
     */
    void cleanup_connection();

    /// OPEN: 내부 타겟 연결 + 세션 생성 + OPEN_ACK 송신
    void handle_open(const TunnelFrame& frame);

    /// DATA: 해당 세션의 target_fd로 페이로드 전달
    void handle_data(const TunnelFrame& frame);

    /// CLOSE: 세션 종료 + 자원 정리
    void handle_close(const TunnelFrame& frame);

    /**
     * 세션 종료 + 자원 정리
     *
     * sessions_mutex_ 보호 하에 세션을 맵에서 제거.
     * target_fd shutdown + close → reader 스레드 recv 에러 → 스레드 종료.
     * reader.join()으로 스레드 완전 종료 확인.
     */
    void close_session(uint32_t session_id);

    // ── 스레드 함수 ───────────────────────────────────────────────────────────

    /**
     * per-session reader 스레드 함수
     *
     * target_fd에서 데이터를 읽어 DATA 프레임으로 server에 전달.
     * EOF/오류 감지 시:
     *   1. CLOSE 프레임을 server에 송신
     *   2. sessions_ 맵에서 자신을 제거 (reader.detach() 후 erase)
     *   3. 스레드 함수 반환
     *
     * 왜 close_session()을 호출하지 않는가:
     *   close_session()은 reader.join()을 호출한다.
     *   스레드가 자기 자신을 join하면 데드락이 발생한다.
     *   대신 reader.detach()로 스레드를 분리한 뒤 맵에서 제거한다.
     */
    void target_reader(uint32_t session_id, int target_fd);

    /**
     * HEARTBEAT 전송 스레드 함수
     *
     * heartbeat_interval_s_ 주기로 HEARTBEAT 프레임을 서버에 전송.
     * 1초 단위로 running_을 확인해 stop() 시 빠르게 종료.
     */
    void heartbeat_loop();

    // ── 멤버 변수 ─────────────────────────────────────────────────────────────

    /// 지수 백오프 최대 대기 시간 (초)
    static constexpr int MAX_RECONNECT_DELAY_S = 60;

    std::string server_ip_;
    int         server_port_;
    std::string agent_id_;
    int         heartbeat_interval_s_;
    int         heartbeat_timeout_s_;

    std::atomic<bool>    running_{false};
    int                  server_fd_{-1};  // run() 실행 중에만 유효

    /**
     * 마지막 HEARTBEAT_ACK 수신 시각 (nanoseconds since steady_clock epoch)
     *
     * connect_and_run() 시작 시 현재 시각으로 초기화.
     * HEARTBEAT_ACK 수신 시 갱신.
     * heartbeat_loop()에서 타임아웃 계산에 사용.
     *
     * int64_t atomic: std::atomic<time_point>는 표준 미지원.
     * 0 = 미초기화 (run() 전).
     */
    std::atomic<int64_t> last_ack_ns_{0};

    /**
     * 현재 재연결 백오프 딜레이 (초)
     *
     * 연결 중 또는 run() 미호출: 0
     * 재연결 대기 중: 1 ~ MAX_RECONNECT_DELAY_S
     */
    std::atomic<int> current_reconnect_delay_{0};

    TunnelMetrics metrics_;

    std::mutex send_mutex_;             // server_fd 쓰기 직렬화
    mutable std::mutex sessions_mutex_; // sessions_ 맵 보호

    std::unordered_map<uint32_t, std::unique_ptr<Session>> sessions_;
    std::thread heartbeat_thread_;
};

} // namespace proxy
