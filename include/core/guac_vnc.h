#pragma once

#include "core/guac_parser.h"
#include <string>
#include <functional>
#include <memory>
#include <thread>
#include <atomic>

namespace proxy {

/**
 * @file guac_vnc.h
 * @brief Phase 8-D — libvncclient 기반 VNC 클라이언트 + Guacamole 화면 스트리밍
 *
 * ── 역할 ──────────────────────────────────────────────────────────────────
 *
 *   1. libvncclient를 사용해 VNC 서버(호스트:포트)에 TCP 연결 + RFB 핸드셰이크
 *   2. GotFrameBufferUpdate 콜백으로 더티 영역(픽셀 직사각형)을 수신
 *   3. 더티 영역을 PNG로 인코딩
 *   4. PNG를 Guacamole img/blob/end 명령어로 변환해 콜백으로 전달
 *
 * ── 화면 스트리밍 흐름 ────────────────────────────────────────────────────
 *
 *   VNC 서버 → libvncclient RFB 업데이트 → GotFrameBufferUpdate 콜백
 *   → client->frameBuffer (BGRA32 픽셀 버퍼) 더티 영역 추출
 *   → BGRA → RGBA 변환 → zlib PNG 인코딩
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
 * ── 픽셀 포맷 ─────────────────────────────────────────────────────────────
 *
 *   rfbGetClient(8, 3, 4): 32bpp, depth 24
 *   리틀엔디안에서 메모리 순서: B G R X (BGRX)
 *   → PNG 인코딩 전에 BGRA→RGBA 채널 스왑 수행
 *
 * ── 스레드 모델 ───────────────────────────────────────────────────────────
 *
 *   connect()는 VNC 이벤트 루프를 별도 스레드(worker_)에서 실행한다.
 *   InstructionCallback은 worker_ 스레드에서 호출된다.
 *   disconnect()는 종료 플래그를 설정하고 worker_ 스레드 종료를 기다린다.
 *
 * ── libvncclient 타입 은닉 (pImpl + Callbacks 패턴) ───────────────────────
 *
 *   libvncclient 헤더(rfb/rfbclient.h)는 guac_vnc.cpp에서만 include된다.
 *   헤더에는 Callbacks nested struct 포워드 선언만 존재하며,
 *   friend 선언으로 private 멤버 접근 권한을 부여한다.
 */

class GuacVncClient {
public:
    /**
     * InstructionCallback — Guacamole 명령어가 준비될 때마다 호출된다.
     *
     * 연결 수립 시: size 명령어 (캔버스 크기 설정)
     * 화면 업데이트 시: img / blob / end 명령어 시퀀스
     * 콜백은 VNC 이벤트 루프 스레드에서 호출된다.
     */
    using InstructionCallback = std::function<void(const GuacInstruction&)>;

    explicit GuacVncClient(InstructionCallback callback);
    ~GuacVncClient();

    GuacVncClient(const GuacVncClient&)            = delete;
    GuacVncClient& operator=(const GuacVncClient&) = delete;

    /**
     * VNC 서버에 연결한다.
     *
     * 비동기 — 연결 + 이벤트 루프를 백그라운드 스레드(worker_)에서 실행.
     * 연결 성공 시 is_connected()가 true를 반환.
     *
     * @param host     VNC 서버 IPv4 주소 또는 호스트명
     * @param port     VNC 포트 (일반적으로 5900)
     * @param password VNC 비밀번호 (빈 문자열이면 인증 없음)
     */
    void connect(const std::string& host, uint16_t port,
                 const std::string& password = "");

    /**
     * VNC 연결을 종료한다.
     *
     * 종료 플래그를 설정하고 worker_ 스레드가 끝날 때까지 블로킹 대기.
     * 이미 연결이 끊어진 상태에서 호출해도 안전하다.
     */
    void disconnect();

    /** @return 현재 VNC 서버와 연결 중이면 true */
    bool is_connected() const;

private:
    // libvncclient 정적 콜백이 private 멤버에 접근할 수 있도록 friend 선언
    // Callbacks는 guac_vnc.cpp에서 정의됨 (rfbclient.h를 헤더에 노출하지 않기 위함)
    struct Callbacks;
    friend struct Callbacks;

    // libvncclient 타입을 숨기기 위한 pImpl
    struct Impl;

    InstructionCallback   callback_;
    std::atomic<bool>     connected_{false};
    std::thread           worker_;
    std::unique_ptr<Impl> impl_;
    int                   next_stream_id_{1};  // Guacamole 스트림 ID 카운터
    std::string           password_;           // GetPassword 콜백에서 참조

    /**
     * run_event_loop — worker_ 스레드 진입점
     *
     * rfbGetClient()로 클라이언트를 초기화하고, rfbInitClient()로 서버에 연결한 뒤,
     * WaitForMessage() + HandleRFBServerMessage() 루프를 실행한다.
     * impl_->stop이 true가 되거나 연결이 끊어지면 종료한다.
     */
    void run_event_loop(const std::string& host, uint16_t port,
                        const std::string& password);

    /**
     * flush_dirty_region — GotFrameBufferUpdate 콜백(Callbacks::got_update)에서 호출된다.
     *
     * framebuffer의 더티 영역을 PNG로 인코딩하고
     * img → blob... → end GuacInstruction 시퀀스를 callback_으로 전달한다.
     *
     * @param framebuffer  BGRX32 포맷 픽셀 버퍼 (rfbClient->frameBuffer)
     * @param stride       scanline 간 바이트 수 (width * bytes_per_pixel)
     * @param x/y          더티 영역 좌상단 좌표
     * @param w/h          더티 영역 너비/높이
     */
    void flush_dirty_region(const uint8_t* framebuffer, int stride,
                            int x, int y, int w, int h);
};

} // namespace proxy
