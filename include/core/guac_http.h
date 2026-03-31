#pragma once

#include "core/guac_parser.h"
#include <string>
#include <functional>
#include <memory>
#include <thread>
#include <atomic>

namespace proxy {

/**
 * @file guac_http.h
 * @brief Phase 14-A — libcurl 기반 HTTP 클라이언트 (web 프로토콜 기본 구현)
 *
 * ── 역할 ──────────────────────────────────────────────────────────────────
 *
 *   Guacamole `web` 프로토콜의 기본 구현체.
 *   지정한 URL을 libcurl로 HTTP GET하고 응답을 `response` instruction으로 반환한다.
 *
 *   GuacWebClient(Chromium CDP)와 동일한 InstructionCallback 인터페이스를 공유하므로
 *   GuacWebSocketGateway에서 web_renderer 설정에 따라 투명하게 교체 가능하다.
 *
 * ── Guacamole response instruction ───────────────────────────────────────
 *
 *   connect,web,https://내부서버/path;  ← 브라우저 → 게이트웨이
 *   response,200,text/html,<base64>;   ← 게이트웨이 → 브라우저
 *
 *   형식: response,<status_code>,<content_type>,<base64_encoded_body>;
 *
 * ── GuacWebClient와의 구조적 유사성 ──────────────────────────────────────
 *
 *   - connect() → 백그라운드 스레드(worker_)에서 HTTP 요청 실행
 *   - InstructionCallback → response instruction 전달
 *   - disconnect() → 진행 중인 요청 중단 + worker_ 조인
 *
 *   HTTP는 일회성(one-shot) 요청이므로 send_mouse / send_key는 지원하지 않는다.
 *
 * ── 스레드 모델 ───────────────────────────────────────────────────────────
 *
 *   connect()는 백그라운드 스레드(worker_)를 시작하고 즉시 반환한다.
 *   worker_는 curl_easy_perform()으로 응답 수집 후 callback_을 호출하고 종료한다.
 *   disconnect()는 abort_ 플래그를 설정하고 worker_ 종료를 대기한다.
 */
class GuacHttpClient {
public:
    /**
     * InstructionCallback — response instruction이 준비되면 호출된다.
     * 콜백은 worker_ 스레드에서 호출된다.
     */
    using InstructionCallback = std::function<void(const GuacInstruction&)>;

    explicit GuacHttpClient(InstructionCallback callback);
    ~GuacHttpClient();

    GuacHttpClient(const GuacHttpClient&)            = delete;
    GuacHttpClient& operator=(const GuacHttpClient&) = delete;

    /**
     * 지정한 URL을 HTTP GET으로 요청한다.
     *
     * 비동기 — worker_ 스레드에서 요청을 처리한다.
     * 요청 완료 시 response instruction을 콜백으로 전달하고 is_connected()가 false로 전환된다.
     *
     * @param url  요청할 URL (예: "https://example.com")
     */
    void connect(const std::string& url);

    /**
     * 진행 중인 요청을 중단하고 worker_ 스레드 종료를 대기한다.
     * 이미 완료되거나 연결되지 않은 상태에서 호출해도 안전하다.
     */
    void disconnect();

    /** @return worker_ 스레드가 실행 중이면 true */
    bool is_connected() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    InstructionCallback callback_;
    std::atomic<bool>   connected_{false};
    std::atomic<bool>   abort_{false};
    std::thread         worker_;

    /**
     * run_request — worker_ 스레드 진입점
     *
     * 실행 순서:
     *   1. curl_easy_init()으로 CURL 핸들 초기화
     *   2. curl_easy_perform()으로 HTTP GET 수행 (응답 바디를 메모리로 수집)
     *   3. HTTP 상태 코드와 Content-Type 헤더 추출
     *   4. 응답 바디를 base64 인코딩
     *   5. response instruction 생성 및 콜백 호출
     *   6. connected_ = false로 전환
     *
     * @param url  요청할 URL
     */
    void run_request(const std::string& url);
};

} // namespace proxy
