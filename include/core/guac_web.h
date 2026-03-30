#pragma once

#include "core/guac_parser.h"
#include <string>
#include <functional>
#include <memory>
#include <thread>
#include <atomic>
#include <sys/types.h>  // pid_t

namespace proxy {

/**
 * @file guac_web.h
 * @brief Phase 13-A/B/C — Chrome headless + CDP + JPEG 스트리밍 + 마우스/키보드 입력
 *
 * ── 역할 ──────────────────────────────────────────────────────────────────
 *
 *   1. Chromium을 headless 모드로 fork/exec해 CDP 포트를 연다
 *   2. HTTP GET /json으로 CDP WebSocket URL을 획득한다
 *   3. CDP WebSocket(RFC 6455 클라이언트)으로 Chrome에 연결한다
 *   4. Page.navigate를 전송해 지정 URL의 페이지를 로드한다
 *   5. Page.loadEventFired 이벤트를 수신해 로드 완료를 확인한다
 *   6. Page.captureScreenshot 루프로 JPEG 스크린샷을 캡처한다       [Phase 13-B]
 *   7. JPEG를 Guacamole img/blob/end 명령어로 변환해 콜백으로 전달한다  [Phase 13-B]
 *   8. 마우스/키보드 입력을 CDP Input 이벤트로 Chrome에 주입한다     [Phase 13-C]
 *
 * ── GuacVncClient와의 구조적 유사성 ──────────────────────────────────────
 *
 *   GuacWebClient는 GuacVncClient와 동일한 패턴으로 설계된다:
 *   - connect() → 백그라운드 스레드(worker_)에서 이벤트 루프 실행
 *   - InstructionCallback → img/blob/end Guacamole 명령어 스트리밍
 *   - disconnect() → 종료 플래그 + worker_ 조인
 *
 *   픽셀 소스만 libvncclient(RFB 업데이트 콜백) 대신
 *   CDP `Page.captureScreenshot` 응답으로 교체한다.
 *
 * ── CDP (Chrome DevTools Protocol) ───────────────────────────────────────
 *
 *   Chrome에 `--remote-debugging-port=PORT` 플래그를 주면
 *   `ws://localhost:PORT/devtools/page/ID`에 JSON-over-WebSocket API가 열린다.
 *
 *   CDP 메시지 형식:
 *     요청:  {"id": N, "method": "Domain.method", "params": {...}}
 *     응답:  {"id": N, "result": {...}}
 *     이벤트: {"method": "Domain.event", "params": {...}}
 *
 * ── Chromium 탐지 ─────────────────────────────────────────────────────────
 *
 *   which chromium-browser → which chromium → which google-chrome 순으로 탐지.
 *   설치: apt install chromium-browser
 *
 * ── CDP WebSocket 프레임 마스킹 ───────────────────────────────────────────
 *
 *   클라이언트(우리) → Chrome(서버): RFC 6455 §5.3에 따라 마스킹 필수
 *   Chrome(서버) → 클라이언트(우리): 마스킹 없음
 *
 * ── 스레드 모델 ───────────────────────────────────────────────────────────
 *
 *   connect()는 백그라운드 스레드(worker_)를 시작하고 즉시 반환한다.
 *   worker_는 Chrome 실행 → CDP 연결 → 페이지 로드 → (Phase 13-B) 캡처 루프를 담당한다.
 *   disconnect()는 종료 플래그를 설정하고 worker_ 종료를 대기한 뒤 Chrome을 종료한다.
 *
 * ── 입력 처리 스레드 모델 ────────────────────────────────────────────────
 *
 *   send_mouse() / send_key()는 guac_websocket의 입력 루프 스레드에서 호출된다.
 *   cdp_send()는 worker_ 스레드의 captureScreenshot 루프에서만 사용한다.
 *   두 스레드의 CDP WebSocket 쓰기 충돌을 피하기 위해 입력 큐(input_queue_) 방식을 사용한다:
 *
 *   send_mouse/send_key → input_queue_ (push, mutex 보호)
 *   captureScreenshot 루프 반복 시작 → drain_input_queue() → CDP 전송 (worker_ 스레드)
 */

class GuacWebClient {
public:
    /**
     * InstructionCallback — Guacamole 명령어가 준비될 때마다 호출된다.
     *
     * 연결 수립 시: size 명령어 (캔버스 크기 설정)
     * 매 프레임:   img / blob... / end 명령어 시퀀스 (JPEG 스크린샷)
     * 콜백은 worker_ 스레드에서 호출된다.
     */
    using InstructionCallback = std::function<void(const GuacInstruction&)>;

    explicit GuacWebClient(InstructionCallback callback);
    ~GuacWebClient();

    GuacWebClient(const GuacWebClient&)            = delete;
    GuacWebClient& operator=(const GuacWebClient&) = delete;

    /**
     * 지정한 URL을 headless Chrome으로 연다.
     *
     * 비동기 — Chrome 실행 + CDP 연결을 백그라운드 스레드에서 처리.
     * 연결 성공 시 is_connected()가 true를 반환.
     *
     * @param url    로드할 URL (예: "https://example.com")
     * @param width    캔버스 너비 (기본 1280)
     * @param height   캔버스 높이 (기본 800)
     * @param cdp_port Chrome CDP 포트 (기본 9222). 테스트 시 MockCdpServer 포트를 지정한다.
     *                 지정한 포트가 이미 열려 있으면 Chrome fork를 생략한다.
     */
    void connect(const std::string& url, int width = 1280, int height = 800, int cdp_port = 9222);

    /**
     * Chrome 연결을 종료하고 프로세스를 종료한다.
     *
     * worker_ 스레드 종료 후 SIGTERM → 500ms 대기 → SIGKILL 순서로 Chrome을 정리한다.
     * 이미 연결이 끊어진 상태에서 호출해도 안전하다.
     */
    void disconnect();

    /** @return Chrome이 실행 중이고 CDP 연결이 수립된 상태이면 true */
    bool is_connected() const;

    /**
     * send_mouse — Guacamole mouse 명령어를 CDP Input.dispatchMouseEvent로 변환해 큐에 넣는다.
     *
     * Guacamole button_mask 비트 정의:
     *   bit 0 (1):  왼쪽 버튼 pressed
     *   bit 1 (2):  가운데 버튼 pressed
     *   bit 2 (4):  오른쪽 버튼 pressed
     *   bit 3 (8):  스크롤 위 (순간적)
     *   bit 4 (16): 스크롤 아래 (순간적)
     *
     * 이전 button_mask와 비교해 mousePressed / mouseReleased / mouseMoved / mouseWheel을
     * 적절히 생성하고 input_queue_에 push한다. 실제 CDP 전송은 worker_ 스레드가 담당한다.
     *
     * @param x           마우스 X 좌표 (뷰포트 기준)
     * @param y           마우스 Y 좌표 (뷰포트 기준)
     * @param button_mask Guacamole 버튼 상태 비트마스크
     */
    void send_mouse(int x, int y, int button_mask);

    /**
     * send_key — Guacamole key 명령어를 CDP Input.dispatchKeyEvent로 변환해 큐에 넣는다.
     *
     * X11 keysym → DOM KeyValue 변환:
     *   - 0x20~0x7E: ASCII printable (space ~ tilde)
     *   - 0xFF08: Backspace, 0xFF09: Tab, 0xFF0D: Enter, 0xFF1B: Escape
     *   - 0xFF50~0xFF57: Home/Arrow/Page/End
     *   - 0xFFBE~0xFFC9: F1~F12
     *   - 0xFFE1~0xFFEA: Shift/Control/Alt/Meta 수정자 키
     *
     * @param keysym  X11 keysym 코드
     * @param pressed true = 키 누름(keyDown), false = 키 해제(keyUp)
     */
    void send_key(int keysym, bool pressed);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    InstructionCallback callback_;
    std::atomic<bool>   connected_{false};
    std::thread         worker_;

    /**
     * find_chromium — 시스템에서 Chromium 실행 파일 경로를 탐색한다.
     *
     * chromium-browser → chromium → google-chrome 순서로 which를 시도한다.
     *
     * @return 실행 파일 절대 경로. 없으면 빈 문자열.
     */
    static std::string find_chromium();

    /**
     * fork_chromium — headless Chrome 프로세스를 fork/exec으로 시작한다.
     *
     * 전달되는 플래그:
     *   --headless=new           새 헤드리스 모드 (Chrome 112+)
     *   --disable-gpu            GPU 렌더링 비활성화 (서버 환경)
     *   --no-sandbox             root 실행 환경 대응
     *   --disable-dev-shm-usage  /dev/shm 용량 부족 방지
     *   --remote-debugging-port  CDP 포트 개방
     *   --window-size            뷰포트 크기 설정
     *
     * @param cdp_port  Chrome에 열어줄 CDP 포트 번호
     * @param width     뷰포트 너비
     * @param height    뷰포트 높이
     * @return 자식 프로세스 PID. 실패 시 -1.
     */
    pid_t fork_chromium(int cdp_port, int width, int height);

    /**
     * get_cdp_ws_url — Chrome의 HTTP /json 엔드포인트에서 CDP WebSocket URL을 얻는다.
     *
     * Chrome 시작 직후에는 포트가 아직 열리지 않을 수 있으므로
     * timeout_ms 동안 200ms 간격으로 재시도한다.
     *
     * 응답 JSON 형식 (배열):
     *   [{"type":"page","webSocketDebuggerUrl":"ws://localhost:PORT/devtools/page/ID",...}]
     *
     * @param cdp_port    Chrome CDP 포트
     * @param timeout_ms  재시도 타임아웃 (밀리초, 기본 5000)
     * @return CDP WebSocket URL ("ws://localhost:PORT/devtools/page/ID")
     * @throws std::runtime_error 타임아웃 또는 파싱 실패 시
     */
    std::string get_cdp_ws_url(int cdp_port, int timeout_ms = 5000);

    /**
     * connect_cdp_ws — CDP WebSocket URL에 RFC 6455 클라이언트 핸드셰이크로 연결한다.
     *
     * URL 형식: "ws://HOST:PORT/PATH"
     * Sec-WebSocket-Key는 /dev/urandom에서 읽은 16바이트를 base64 인코딩한 값.
     *
     * @param ws_url CDP WebSocket URL
     * @return 연결된 TCP fd. 실패 시 -1.
     */
    int connect_cdp_ws(const std::string& ws_url);

    /**
     * cdp_send — CDP JSON 요청을 WebSocket Text 프레임으로 전송한다.
     *
     * 클라이언트→서버 프레임이므로 RFC 6455 §5.3에 따라 마스킹한다.
     * 마스킹 키는 /dev/urandom에서 4바이트로 생성한다.
     *
     * @param ws_fd   CDP WebSocket fd
     * @param id      요청 ID (응답 매칭용)
     * @param method  CDP 메서드 이름 (예: "Page.navigate")
     * @param params  JSON 파라미터 문자열 (기본: "{}")
     * @return 전송 성공이면 true
     */
    bool cdp_send(int ws_fd, int id, const std::string& method,
                  const std::string& params = "{}");

    /**
     * cdp_recv — WebSocket 프레임을 하나 읽어 페이로드 문자열로 반환한다.
     *
     * Chrome(서버)→클라이언트 프레임이므로 마스킹 없음.
     * opcode=9(Ping) 수신 시 Pong을 즉시 전송하고 다음 프레임을 계속 읽는다.
     *
     * @param ws_fd  CDP WebSocket fd
     * @return 페이로드 JSON 문자열. 연결 종료(opcode=8 또는 recv 실패) 시 빈 문자열.
     */
    std::string cdp_recv(int ws_fd);

    /**
     * drain_input_queue — input_queue_의 이벤트를 CDP로 전송하고 응답을 소비한다.
     *
     * worker_ 스레드에서만 호출된다. CDP 쓰기는 single-threaded이므로 별도 mutex 불필요.
     * 큐 접근만 input_mutex_로 보호한다.
     *
     * @param ws_fd    CDP WebSocket fd
     * @param next_id  CDP 요청 ID (in-out, 함수 종료 시 다음 사용 가능한 ID로 업데이트)
     * @return 연결이 유지되면 true, 끊어지면 false
     */
    bool drain_input_queue(int ws_fd, int& next_id);

    /**
     * make_key_params — keysym과 pressed 상태를 CDP Input.dispatchKeyEvent 파라미터 JSON으로 변환한다.
     *
     * @param keysym  X11 keysym 코드
     * @param pressed keyDown이면 true, keyUp이면 false
     * @return CDP params JSON 문자열
     */
    static std::string make_key_params(int keysym, bool pressed);

    /**
     * flush_delta — 전 프레임과 비교해 변경된 영역만 JPEG 재인코딩해 전송한다.
     *
     * Phase 13-D delta 압축 알고리즘:
     *   1. b64_jpeg를 base64 decode → stb_image로 RGB 픽셀 디코딩
     *   2. Impl::prev_pixels(이전 프레임)와 픽셀 단위 비교
     *      - 변화 없음: 전송 스킵 (연속 동일 프레임 대역폭 0)
     *      - 변화 있음: dirty bounding rect (min/max 변화 행·열) 계산
     *   3. dirty rect가 전체의 75% 미만이면 해당 영역만 JPEG 재인코딩 전송
     *      (75% 이상이면 원본 b64_jpeg 그대로 사용 — 재인코딩 오버헤드 불필요)
     *   4. img instruction의 x/y를 dirty rect 오프셋으로 설정 → 캔버스 부분 업데이트
     *   5. 현재 픽셀을 Impl::prev_pixels에 저장 (다음 프레임 비교용)
     *
     * @param b64_jpeg   base64 인코딩된 JPEG 이미지 문자열
     * @param stream_id  Guacamole 스트림 ID (Impl::next_stream_id에서 발급)
     */
    void flush_delta(const std::string& b64_jpeg, int stream_id);

    /**
     * run_event_loop — worker_ 스레드 진입점
     *
     * 실행 순서:
     *   1. fork_chromium()으로 Chrome 실행
     *   2. get_cdp_ws_url()로 CDP WS URL 획득 (최대 5초 재시도)
     *   3. connect_cdp_ws()로 CDP 연결
     *   4. Page.enable 전송 (Page 이벤트 구독)
     *   5. Page.navigate 전송 (URL 로드)
     *   6. Page.loadEventFired 이벤트 수신 → size 명령어 콜백 → connected_ = true
     *   7. Page.captureScreenshot 루프 → flush_screenshot() → img/blob/end 콜백
     *
     * @param url    로드할 URL
     * @param width  뷰포트 너비
     * @param height 뷰포트 높이
     */
    void run_event_loop(const std::string& url, int width, int height);
};

} // namespace proxy
