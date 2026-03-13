#pragma once

#include "core/guac_parser.h"
#include "core/guac_rdp.h"
#include "core/guac_ssh.h"
#include "core/guac_vnc.h"

#include <memory>
#include <thread>
#include <atomic>

namespace proxy {

/**
 * @file guac_websocket.h
 * @brief Phase 8-E — 브라우저 WebSocket 연결 + Guacamole 스트림 연결
 *
 * ── 역할 ──────────────────────────────────────────────────────────────────
 *
 *   1. TCP 포트를 열고 브라우저의 WebSocket 연결을 수락한다 (RFC 6455)
 *   2. 첫 번째 Guacamole 명령어가 "connect"이면 프로토콜 타입에 따라
 *      GuacRdpClient / GuacSshClient / GuacVncClient를 생성해 백엔드에 연결한다
 *   3. 백엔드 InstructionCallback → WebSocket 텍스트 프레임으로 전달 (서버→브라우저)
 *   4. WebSocket 텍스트 프레임 → Guacamole 명령어로 파싱 → 백엔드로 전달 (브라우저→서버)
 *
 * ── connect instruction 형식 ─────────────────────────────────────────────
 *
 *   rdp: "connect", "rdp", host, port, username, password
 *   ssh: "connect", "ssh", host, port, username, password
 *   vnc: "connect", "vnc", host, port, password
 *
 * ── WebSocket 프레임 포맷 (RFC 6455) ────────────────────────────────────
 *
 *   클라이언트→서버 프레임: MASK 비트 ON, 4바이트 마스킹 키 포함
 *   서버→클라이언트 프레임: MASK 비트 OFF
 *   Guacamole 명령어는 텍스트 프레임(opcode=0x01)으로 전송된다.
 *
 * ── 스레드 모델 ───────────────────────────────────────────────────────────
 *
 *   start()는 accept_loop를 accept_thread_에서 실행한다.
 *   accept_loop는 새 연결마다 handle_connection을 detach된 스레드로 실행한다.
 *   stop()은 리슨 소켓을 닫아 accept()를 깨우고 accept_thread_가 끝날 때까지 대기한다.
 */

class GuacWebSocketGateway {
public:
    GuacWebSocketGateway();
    ~GuacWebSocketGateway();

    GuacWebSocketGateway(const GuacWebSocketGateway&)            = delete;
    GuacWebSocketGateway& operator=(const GuacWebSocketGateway&) = delete;

    /**
     * 지정한 포트에서 WebSocket 연결 수신을 시작한다.
     * @throws std::runtime_error bind/listen 실패 시
     */
    void start(uint16_t port);

    /**
     * 수신을 중단하고 리슨 소켓을 닫는다.
     * accept_thread_가 종료될 때까지 블로킹 대기.
     * 이미 중단된 상태에서 호출해도 안전하다.
     */
    void stop();

    /** @return start() 이후 stop() 전이면 true */
    bool is_running() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::atomic<bool>     running_{false};
    int                   listen_fd_{-1};
    std::thread           accept_thread_;

    void accept_loop();
    void handle_connection(int fd);
};

} // namespace proxy
