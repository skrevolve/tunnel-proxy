#pragma once

#include <string>

/**
 * @file config.h
 * @brief JSON 설정 파일 파서
 *
 * config.json 예시:
 * @code
 * {
 *   "local_port":  8080,        // 프록시가 리스닝할 포트
 *   "target_ip":   "127.0.0.1", // 트래픽을 전달할 대상 서버 IP
 *   "target_port": 8000,        // 트래픽을 전달할 대상 서버 포트
 *   "mode":        "tcp",       // 프로토콜 모드 (현재 tcp만 지원)
 *   "verbose":     true,        // true면 DEBUG 레벨까지 출력
 *   "log_file":    "proxy.log"  // 로그 파일 경로 (빈 문자열이면 파일 출력 안 함)
 * }
 * @endcode
 *
 * 설계 결정:
 *   - 정적 팩토리 메서드(load_from_file)를 쓰는 이유:
 *     생성자에서 파일 I/O와 파싱 실패가 생기면 예외를 던져야 하는데,
 *     생성자 예외는 처리하기 불편하다. 정적 팩토리는 실패 시 명확하게
 *     예외를 던지고, 성공 시 완전히 초기화된 객체를 반환한다.
 *
 *   - 멤버 변수에 직접 접근하지 않고 getter를 쓰는 이유:
 *     나중에 값 검증 로직(포트 범위 확인 등)을 getter 안에 추가하거나,
 *     내부 표현을 바꾸더라도 외부 코드를 수정하지 않아도 된다.
 */
class Config {
public:
    /**
     * JSON 파일에서 설정을 읽어 Config 객체를 생성한다.
     *
     * @param path 설정 파일 경로 (절대경로 또는 실행 디렉토리 기준 상대경로)
     * @return 파싱된 Config 객체
     * @throws std::runtime_error 파일을 열 수 없거나 JSON 형식이 잘못된 경우
     * @throws nlohmann::json::out_of_range 필수 필드(local_port 등)가 없는 경우
     */
    static Config load_from_file(const std::string& path);

    // ── Getter ────────────────────────────────────────────────────────────────

    /** 프록시가 클라이언트 연결을 기다리는 로컬 포트 */
    int get_local_port() const { return local_port_; }

    /** 트래픽을 전달할 대상 서버 IP 주소 */
    std::string get_target_ip() const { return target_ip_; }

    /** 트래픽을 전달할 대상 서버 포트 */
    int get_target_port() const { return target_port_; }

    /** 프로토콜 모드. 현재 "tcp"만 지원, Phase 5에서 "udp" 추가 예정 */
    std::string get_mode() const { return mode_; }

    /** true면 Logger 레벨을 DEBUG로 설정해 상세 로그를 출력한다 */
    bool is_verbose() const { return verbose_; }

    /** 로그를 기록할 파일 경로. 빈 문자열이면 콘솔에만 출력 */
    std::string get_log_file() const { return log_file_; }

    // ── TLS 모드 (mode = "tls") ───────────────────────────────────────────────
    /** 서버 인증서 경로 (PEM). gen_cert.sh로 생성 */
    std::string get_cert_file() const { return cert_file_; }
    /** 서버 개인키 경로 (PEM) */
    std::string get_key_file() const { return key_file_; }

    // ── 터널 서버 모드 (mode = "tunnel-server") ───────────────────────────────
    /** 에이전트 역방향 연결 수신 포트 (기본 9900) */
    int get_agent_port() const { return agent_port_; }
    /** 외부 클라이언트 연결 수신 포트 (0이면 비활성) */
    int get_proxy_port() const { return proxy_port_; }

    // ── 터널 에이전트 모드 (mode = "tunnel-agent") ───────────────────────────
    /** 터널 서버 IP */
    std::string get_server_ip() const { return server_ip_; }
    /** 에이전트 고유 식별자 */
    std::string get_agent_id() const { return agent_id_; }

private:
    // Config 객체는 load_from_file()로만 생성 가능하도록
    // 기본 생성자를 private으로 유지한다.
    Config() = default;

    int         local_port_;   // 필수 필드 — 없으면 예외 발생
    std::string target_ip_;    // 필수 필드 — 없으면 예외 발생
    int         target_port_;  // 필수 필드 — 없으면 예외 발생
    std::string mode_;         // 선택 필드 — 기본값 "tcp"
    bool        verbose_;      // 선택 필드 — 기본값 false
    std::string log_file_;     // 선택 필드 — 기본값 "" (파일 출력 없음)

    // TLS
    std::string cert_file_;    // 선택 — 기본값 "certs/server.crt"
    std::string key_file_;     // 선택 — 기본값 "certs/server.key"

    // tunnel-server
    int agent_port_ = 9900;    // 선택 — 기본값 9900
    int proxy_port_ = 0;       // 선택 — 기본값 0 (비활성)

    // tunnel-agent
    std::string server_ip_;    // 선택 — 기본값 "127.0.0.1"
    std::string agent_id_;     // 선택 — 기본값 "agent-1"
};
