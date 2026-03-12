#pragma once

#include "core/guac_parser.h"
#include <string>
#include <functional>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>

namespace proxy {

/**
 * @file guac_ssh.h
 * @brief Phase 8-C — libssh2 기반 SSH 클라이언트 + Guacamole 터미널 스트리밍
 *
 * ── 역할 ──────────────────────────────────────────────────────────────────
 *
 *   1. libssh2를 사용해 SSH 서버(호스트:포트)에 TCP 연결 + SSH 핸드셰이크
 *   2. 비밀번호 인증 후 PTY 요청 + 셸 채널 오픈
 *   3. 터미널 출력을 Guacamole pipe/blob/end 명령어 시퀀스로 변환해 콜백 전달
 *   4. 키보드 입력을 SSH 채널에 write
 *
 * ── 터미널 스트리밍 흐름 ──────────────────────────────────────────────────
 *
 *   SSH 서버 → libssh2_channel_read() → 터미널 바이트 스트림
 *   → base64 인코딩
 *   → GuacInstruction { "blob", stream_id, <base64-data> } 생성
 *   → InstructionCallback 호출
 *
 * ── Guacamole pipe 스트림 프로토콜 ───────────────────────────────────────
 *
 *   연결 수립 시:
 *     size instruction: "size", stream_id, cols, rows  (터미널 크기)
 *     pipe instruction: "pipe", stream_id, name, mimetype
 *                       → "pipe", "0", "terminal", "text/plain"
 *   터미널 출력 시:
 *     blob instruction: "blob", stream_id, <base64-data>  (터미널 바이트)
 *   연결 종료 시:
 *     end  instruction: "end",  stream_id
 *
 * ── 키보드 입력 ──────────────────────────────────────────────────────────
 *
 *   send_input()은 libssh2_channel_write()로 SSH 채널에 바이트를 직접 씀.
 *   쓰기는 channel_mutex_로 직렬화한다.
 *
 * ── 스레드 모델 ───────────────────────────────────────────────────────────
 *
 *   connect()는 SSH 이벤트 루프를 별도 스레드(worker_)에서 실행한다.
 *   InstructionCallback은 worker_ 스레드에서 호출된다.
 *   disconnect()는 채널/세션에 종료 신호를 보내고 worker_ 종료를 기다린다.
 *
 * ── libssh2 타입 은닉 (pImpl 패턴) ───────────────────────────────────────
 *
 *   libssh2 헤더는 guac_ssh.cpp에서만 include된다.
 *   헤더에는 Impl 전방 선언만 존재한다.
 */

class GuacSshClient {
public:
    /**
     * InstructionCallback — Guacamole 명령어가 준비될 때마다 호출된다.
     *
     * 연결 수립 시: size / pipe 명령어
     * 터미널 출력 시: blob 명령어 시퀀스
     * 연결 종료 시: end 명령어
     * 콜백은 worker_ 스레드에서 호출된다.
     */
    using InstructionCallback = std::function<void(const GuacInstruction&)>;

    explicit GuacSshClient(InstructionCallback callback);
    ~GuacSshClient();

    GuacSshClient(const GuacSshClient&)            = delete;
    GuacSshClient& operator=(const GuacSshClient&) = delete;

    /**
     * SSH 서버에 연결한다.
     *
     * 비동기 — 연결 + 이벤트 루프를 백그라운드 스레드(worker_)에서 실행.
     * 연결 성공 시 is_connected()가 true를 반환.
     *
     * @param host     SSH 서버 IPv4 주소 또는 호스트명
     * @param port     SSH 포트 (일반적으로 22)
     * @param username 사용자 이름
     * @param password 비밀번호
     * @param cols     터미널 열 수 (기본 80)
     * @param rows     터미널 행 수 (기본 24)
     */
    void connect(const std::string& host, uint16_t port,
                 const std::string& username, const std::string& password,
                 uint16_t cols = 80, uint16_t rows = 24);

    /**
     * SSH 연결을 종료한다.
     *
     * 채널 EOF를 보내고 worker_ 스레드가 끝날 때까지 블로킹 대기.
     * 이미 연결이 끊어진 상태에서 호출해도 안전하다.
     */
    void disconnect();

    /** @return 현재 SSH 서버와 연결 중이면 true */
    bool is_connected() const;

    /**
     * 키보드 입력을 SSH 채널로 전송한다.
     *
     * worker_ 스레드와 외부 스레드가 동시에 호출할 수 있으므로
     * channel_mutex_로 쓰기를 직렬화한다.
     *
     * @param data  전송할 바이트열 (UTF-8 터미널 입력)
     */
    void send_input(const std::string& data);

private:
    // libssh2 타입을 숨기기 위한 pImpl
    struct Impl;

    InstructionCallback   callback_;
    std::atomic<bool>     connected_{false};
    std::thread           worker_;
    std::unique_ptr<Impl> impl_;
    std::mutex            channel_mutex_;  // libssh2_channel_write 직렬화
    int                   stream_id_{0};   // Guacamole 터미널 스트림 ID

    /**
     * run_event_loop — worker_ 스레드 진입점
     *
     * libssh2 세션을 초기화하고, 서버에 연결한 뒤,
     * 채널이 EOF가 되거나 disconnect()가 호출될 때까지
     * libssh2_channel_read() 루프를 실행한다.
     */
    void run_event_loop(const std::string& host, uint16_t port,
                        const std::string& username,
                        const std::string& password,
                        uint16_t cols, uint16_t rows);

    /**
     * flush_terminal_output — worker_ 루프에서 호출된다.
     *
     * 터미널 바이트 스트림을 base64 인코딩 후
     * blob GuacInstruction을 callback_으로 전달한다.
     *
     * @param data  libssh2_channel_read()가 반환한 터미널 바이트
     * @param len   바이트 수
     */
    void flush_terminal_output(const char* data, size_t len);
};

} // namespace proxy
