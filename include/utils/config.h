#pragma once

#include <string>

/**
 * @file config.h
 * @brief JSON 설정 파일 파서
 *
 * proxy용 config.json 예시:
 * @code
 * {
 *   "agent_port": 9900,      // 에이전트 역방향 연결 수신 포트
 *   "proxy_port": 9901,      // 외부 클라이언트 연결 수신 포트 (터널 진입점)
 *   "guac_port":  8765,      // Guacamole WebSocket 게이트웨이 포트
 *   "verbose":    true,
 *   "log_file":   "proxy.log"
 * }
 * @endcode
 *
 * agent용 agent.json 예시:
 * @code
 * {
 *   "server_ip":   "공개서버IP",  // 터널 서버 IP
 *   "server_port": 9900,          // 터널 서버 agent_port
 *   "agent_id":    "agent-1",     // 에이전트 고유 식별자
 *   "verbose":     false,
 *   "log_file":    "agent.log"
 * }
 * @endcode
 */
class Config {
public:
    static Config load_from_file(const std::string& path);

    // ── proxy 설정 ────────────────────────────────────────────────────────────
    /** 에이전트 역방향 연결 수신 포트 (TunnelServer) */
    int get_agent_port() const { return agent_port_; }
    /** 외부 클라이언트 연결 수신 포트 (TunnelServer 터널 진입점) */
    int get_proxy_port() const { return proxy_port_; }
    /** Guacamole WebSocket 게이트웨이 포트 */
    int get_guac_port()  const { return guac_port_; }

    // ── agent 설정 ────────────────────────────────────────────────────────────
    /** 터널 서버 IP */
    std::string get_server_ip()   const { return server_ip_; }
    /** 터널 서버 포트 (proxy의 agent_port와 일치해야 함) */
    int         get_server_port() const { return server_port_; }
    /** 에이전트 고유 식별자 */
    std::string get_agent_id()    const { return agent_id_; }

    // ── 공통 ──────────────────────────────────────────────────────────────────
    bool        is_verbose()     const { return verbose_; }
    std::string get_log_file()   const { return log_file_; }

private:
    Config() = default;

    // proxy
    int agent_port_ = 9900;
    int proxy_port_ = 9901;
    int guac_port_  = 8765;

    // agent
    std::string server_ip_   = "127.0.0.1";
    int         server_port_ = 9900;
    std::string agent_id_    = "agent-1";

    // 공통
    bool        verbose_  = false;
    std::string log_file_ = "";
};
