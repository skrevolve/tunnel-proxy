#pragma once

#include "core/guac_parser.h"
#include <string>
#include <functional>
#include <memory>
#include <thread>
#include <atomic>

namespace proxy {

/**
 * @file guac_rdp.h
 * @brief Phase 8-B — FreeRDP 기반 RDP 클라이언트 + Guacamole 화면 스트리밍
 *
 * ── 역할 ──────────────────────────────────────────────────────────────────
 *
 *   1. FreeRDP를 사용해 RDP 서버(호스트:포트)에 TCP 연결 + RDP 핸드셰이크
 *   2. FreeRDP GDI 레이어(software rendering)로 화면 업데이트를 픽셀 버퍼로 수신
 *   3. 더티 영역(dirty region)을 PNG로 인코딩
 *   4. PNG를 Guacamole img/blob/end 명령어로 변환해 콜백으로 전달
 *
 * ── 화면 스트리밍 흐름 ────────────────────────────────────────────────────
 *
 *   RDP 서버 → FreeRDP BitmapUpdate → GDI 레이어가 픽셀 버퍼에 그림
 *   → EndPaint 콜백 발생 (더티 영역 좌표 제공)
 *   → 더티 영역 BGRA → RGBA 변환 → zlib PNG 인코딩
 *   → OpenSSL base64 인코딩
 *   → GuacInstruction { "img", ... } / { "blob", ... } / { "end", ... } 생성
 *   → InstructionCallback 호출
 *
 * ── Guacamole img 스트림 프로토콜 ────────────────────────────────────────
 *
 *   img  instruction: "img", stream_id, compositing_op, layer, "image/png", x, y
 *   blob instruction: "blob", stream_id, <base64-PNG-chunk>  (최대 8KB/청크)
 *   end  instruction: "end",  stream_id
 *
 * ── 스레드 모델 ───────────────────────────────────────────────────────────
 *
 *   connect()는 FreeRDP 이벤트 루프를 별도 스레드(worker_)에서 실행한다.
 *   InstructionCallback은 worker_ 스레드에서 호출된다.
 *   disconnect()는 FreeRDP에 종료 신호를 보내고 worker_ 스레드 종료를 기다린다.
 *
 * ── FreeRDP 타입 은닉 (pImpl + Callbacks 패턴) ────────────────────────────
 *
 *   FreeRDP 헤더는 guac_rdp.cpp에서만 include된다.
 *   헤더에는 Callbacks nested struct 포워드 선언만 존재하며,
 *   friend 선언으로 private 멤버 접근 권한을 부여한다.
 */

class GuacRdpClient {
public:
    /**
     * InstructionCallback — Guacamole 명령어가 준비될 때마다 호출된다.
     *
     * 연결 수립 시: size 명령어 (캔버스 크기 설정)
     * 화면 업데이트 시: img / blob / end 명령어 시퀀스
     * 콜백은 FreeRDP 이벤트 루프 스레드에서 호출된다.
     */
    using InstructionCallback = std::function<void(const GuacInstruction&)>;

    explicit GuacRdpClient(InstructionCallback callback);
    ~GuacRdpClient();

    GuacRdpClient(const GuacRdpClient&)            = delete;
    GuacRdpClient& operator=(const GuacRdpClient&) = delete;

    /**
     * RDP 서버에 연결한다.
     *
     * 비동기 — 연결 + 이벤트 루프를 백그라운드 스레드(worker_)에서 실행.
     * 연결 성공 시 is_connected()가 true를 반환.
     *
     * @param host     RDP 서버 IPv4 주소 또는 호스트명
     * @param port     RDP 포트 (일반적으로 3389)
     * @param username 사용자 이름
     * @param password 비밀번호
     * @param width    초기 화면 너비 (픽셀)
     * @param height   초기 화면 높이 (픽셀)
     */
    void connect(const std::string& host, uint16_t port,
                 const std::string& username, const std::string& password,
                 uint16_t width = 1024, uint16_t height = 768);

    /**
     * RDP 연결을 종료한다.
     *
     * FreeRDP에 종료 신호를 보내고 worker_ 스레드가 끝날 때까지 블로킹 대기.
     * 이미 연결이 끊어진 상태에서 호출해도 안전하다.
     */
    void disconnect();

    /** @return 현재 RDP 서버와 연결 중이면 true */
    bool is_connected() const;

private:
    // FreeRDP 정적 콜백이 private 멤버에 접근할 수 있도록 friend 선언
    // Callbacks는 guac_rdp.cpp에서 정의됨 (FreeRDP 헤더를 헤더에 노출하지 않기 위함)
    struct Callbacks;
    friend struct Callbacks;

    // FreeRDP 타입을 숨기기 위한 pImpl
    struct Impl;

    InstructionCallback   callback_;
    std::atomic<bool>     connected_{false};
    std::thread           worker_;
    std::unique_ptr<Impl> impl_;
    int                   next_stream_id_{1};  // Guacamole 스트림 ID 카운터

    /**
     * run_event_loop — worker_ 스레드 진입점
     *
     * FreeRDP 인스턴스를 초기화하고, 서버에 연결한 뒤,
     * freerdp_shall_disconnect_context()가 true를 반환할 때까지
     * WaitForMultipleObjects + freerdp_check_event_handles 루프를 실행한다.
     */
    void run_event_loop(const std::string& host, uint16_t port,
                        const std::string& username,
                        const std::string& password,
                        uint16_t width, uint16_t height);

    /**
     * flush_dirty_region — FreeRDP EndPaint 콜백(Callbacks::end_paint)에서 호출된다.
     *
     * framebuffer의 더티 영역을 PNG로 인코딩하고
     * img → blob... → end GuacInstruction 시퀀스를 callback_으로 전달한다.
     *
     * @param framebuffer  BGRA32 포맷 픽셀 버퍼 (FreeRDP GDI primary_buffer)
     * @param stride       scanline 간 바이트 수 (gdi->stride)
     * @param dirty_x/y    더티 영역 좌상단 좌표
     * @param dirty_w/h    더티 영역 너비/높이
     */
    void flush_dirty_region(const uint8_t* framebuffer, uint32_t stride,
                            int dirty_x, int dirty_y,
                            int dirty_w, int dirty_h);
};

} // namespace proxy
