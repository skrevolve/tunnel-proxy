/**
 * @file guac_web.cpp
 * @brief Phase 13-A/B — Chrome headless + CDP WebSocket + JPEG 스크린샷 스트리밍
 *
 * ── Chrome 실행 흐름 ──────────────────────────────────────────────────────
 *
 *   fork() → 자식:  exec(chromium --headless=new --remote-debugging-port=PORT)
 *             부모:  get_cdp_ws_url() → connect_cdp_ws() → Page.navigate
 *
 * ── get_cdp_ws_url 재시도 전략 ────────────────────────────────────────────
 *
 *   Chrome 시작 직후 TCP 포트가 열리기까지 수백ms~1초가 걸린다.
 *   200ms 간격으로 HTTP GET /json을 재시도하며, 최대 timeout_ms(기본 5초)를 기다린다.
 *   connect() 실패(ECONNREFUSED)와 JSON 파싱 실패를 모두 재시도 대상으로 처리한다.
 *
 * ── CDP WebSocket 마스킹 ──────────────────────────────────────────────────
 *
 *   RFC 6455 §5.3: 클라이언트→서버 프레임은 반드시 마스킹해야 한다.
 *   마스킹 키는 /dev/urandom에서 4바이트를 읽어 사용한다.
 *   Chrome(서버)→클라이언트 방향 프레임은 마스킹 없음이므로 cdp_recv에서 언마스킹 불필요.
 *
 * ── Page.loadEventFired 대기 ─────────────────────────────────────────────
 *
 *   Page.navigate 후 여러 이벤트가 도착한다 (frameNavigated, domContentEventFired 등).
 *   "loadEventFired"가 오면 모든 리소스 로드 완료.
 *   이 시점에 size 명령어를 콜백으로 전달해 브라우저 캔버스를 초기화한다.
 *
 * ── Page.captureScreenshot 루프 ───────────────────────────────────────────
 *
 *   요청-응답 방식: cdp_send(captureScreenshot) → cdp_recv() 대기 → flush_screenshot()
 *   응답 식별: CDP 응답은 {"id": N, "result": {...}}, 이벤트는 {"method": "..."} 형태.
 *             id 필드로 요청/이벤트를 구분하고, 이벤트는 무시하고 계속 대기한다.
 *   JPEG format + quality 80: PNG보다 크기가 작아 스트리밍에 적합.
 *                              Phase 13-D에서 delta 압축으로 개선 예정.
 *
 * ── flush_screenshot: JPEG → Guacamole img/blob/end ─────────────────────
 *
 *   Page.captureScreenshot의 result.data는 이미 base64 인코딩된 JPEG이다.
 *   추가 인코딩 없이 8192자 단위로 청크 분할해 blob instruction으로 전달한다.
 *
 * ── Chrome 종료 전략 ──────────────────────────────────────────────────────
 *
 *   disconnect() → SIGTERM 전송 → 500ms waitpid → 아직 살아있으면 SIGKILL
 *   좀비 프로세스 방지를 위해 waitpid를 반드시 호출한다.
 */

#include "core/guac_web.h"

#include <nlohmann/json.hpp>
#include <openssl/evp.h>  // EVP_EncodeBlock / EVP_DecodeBlock (base64)

// stb_image — JPEG 디코딩 (Phase 13-D delta 압축)
// STB_IMAGE_IMPLEMENTATION은 이 번역 단위에서 한 번만 정의해야 한다.
// stb 헤더는 -Wextra/-Wpedantic 경고를 다수 발생시키므로 진단을 임시 억제한다.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#pragma GCC diagnostic pop

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>

#include <chrono>
#include <thread>
#include <mutex>
#include <queue>
#include <utility>
#include <stdexcept>
#include <cstring>
#include <vector>
#include <string>

namespace proxy {

// ── GuacWebClient::Impl ───────────────────────────────────────────────────

// 입력 큐 항목: CDP 메서드 + params
struct InputEvent {
    std::string method;
    std::string params;
};

struct GuacWebClient::Impl {
    std::atomic<bool>    stop{false};
    pid_t                chrome_pid{-1};      // fork된 Chrome 프로세스 PID
    int                  cdp_ws_fd{-1};       // CDP WebSocket TCP fd
    int                  cdp_port{9222};      // Chrome CDP 포트
    int                  next_stream_id{1};   // Guacamole 스트림 ID 카운터 (단조 증가)

    // 입력 큐 — send_mouse/send_key(외부 스레드)가 push, drain_input_queue(worker_)가 pop
    std::mutex             input_mutex;
    std::queue<InputEvent> input_queue;
    int                    prev_button_mask{0}; // 이전 마우스 버튼 상태 (변화 감지용)

    // delta 압축 — 이전 프레임 RGB 픽셀 버퍼 (Phase 13-D)
    std::vector<uint8_t>   prev_pixels;       // width × height × 3 (RGB24)
    int                    frame_width{0};
    int                    frame_height{0};
};

// ── recv_exact 헬퍼 ───────────────────────────────────────────────────────

// WebSocket 프레임 헤더 등 정확한 바이트 수를 읽는다.
// 반환값: 읽은 바이트 수. 연결 종료 시 0, 에러 시 -1.
static ssize_t recv_exact(int fd, void* buf, size_t n) {
    size_t total = 0;
    auto*  p     = static_cast<char*>(buf);
    while (total < n) {
        ssize_t r = recv(fd, p + total, n - total, MSG_WAITALL);
        if (r <= 0) return r == 0 ? static_cast<ssize_t>(total) : -1;
        total += static_cast<size_t>(r);
    }
    return static_cast<ssize_t>(total);
}

// ── GuacWebClient 생성/소멸 ───────────────────────────────────────────────

GuacWebClient::GuacWebClient(InstructionCallback callback)
    : impl_(std::make_unique<Impl>())
    , callback_(std::move(callback))
{}

GuacWebClient::~GuacWebClient() {
    disconnect();
}

// ── 공개 API ──────────────────────────────────────────────────────────────

void GuacWebClient::connect(const std::string& url, int width, int height) {
    if (connected_.load()) return;
    impl_->stop = false;
    worker_ = std::thread(&GuacWebClient::run_event_loop, this, url, width, height);
}

void GuacWebClient::disconnect() {
    impl_->stop = true;

    // CDP WebSocket fd를 닫아 cdp_recv 블로킹을 해제한다.
    if (impl_->cdp_ws_fd >= 0) {
        shutdown(impl_->cdp_ws_fd, SHUT_RDWR);
        close(impl_->cdp_ws_fd);
        impl_->cdp_ws_fd = -1;
    }

    if (worker_.joinable()) worker_.join();

    // Chrome 프로세스 종료
    if (impl_->chrome_pid > 0) {
        kill(impl_->chrome_pid, SIGTERM);

        // 최대 500ms 동안 자발적 종료를 기다린다.
        for (int i = 0; i < 5; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (waitpid(impl_->chrome_pid, nullptr, WNOHANG) > 0) {
                impl_->chrome_pid = -1;
                return;
            }
        }
        // 자발적 종료를 거부하면 강제 종료한다.
        kill(impl_->chrome_pid, SIGKILL);
        waitpid(impl_->chrome_pid, nullptr, 0);
        impl_->chrome_pid = -1;
    }
}

bool GuacWebClient::is_connected() const {
    return connected_.load();
}

// ── Chromium 탐지 ─────────────────────────────────────────────────────────

std::string GuacWebClient::find_chromium() {
    static const char* candidates[] = {
        "chromium-browser", "chromium", "google-chrome", nullptr
    };
    for (int i = 0; candidates[i]; i++) {
        std::string cmd = std::string("which ") + candidates[i] + " 2>/dev/null";
        FILE* p = popen(cmd.c_str(), "r");
        if (!p) continue;
        char buf[512] = {};
        if (fgets(buf, sizeof(buf) - 1, p) == nullptr) {
            pclose(p);
            continue;
        }
        pclose(p);
        std::string path(buf);
        if (!path.empty() && path.back() == '\n') path.pop_back();
        if (!path.empty()) return path;
    }
    return "";
}

// ── Chrome fork/exec ──────────────────────────────────────────────────────

pid_t GuacWebClient::fork_chromium(int cdp_port, int width, int height) {
    std::string chrome = find_chromium();
    if (chrome.empty()) return -1;

    std::string port_arg = "--remote-debugging-port=" + std::to_string(cdp_port);
    std::string size_arg = "--window-size=" + std::to_string(width) + "," + std::to_string(height);

    pid_t pid = fork();
    if (pid < 0) return -1;

    if (pid == 0) {
        // 자식: stdout/stderr를 /dev/null로 리다이렉트해 출력을 억제한다.
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }

        // Chrome headless 실행
        // --headless=new: Chrome 112+ 신규 헤드리스 모드 (old --headless는 deprecated)
        // --no-sandbox: root 또는 컨테이너 환경에서 sandbox 비활성화
        // --disable-dev-shm-usage: /dev/shm이 작은 환경(Docker 등)에서 크래시 방지
        execlp(chrome.c_str(), chrome.c_str(),
               "--headless=new",
               "--disable-gpu",
               "--no-sandbox",
               "--disable-dev-shm-usage",
               port_arg.c_str(),
               size_arg.c_str(),
               nullptr);
        _exit(1);  // exec 실패 시 _exit (atexit 핸들러 건너뜀)
    }

    return pid;
}

// ── CDP WebSocket URL 획득 ────────────────────────────────────────────────

std::string GuacWebClient::get_cdp_ws_url(int cdp_port, int timeout_ms) {
    using clock = std::chrono::steady_clock;
    auto deadline = clock::now() + std::chrono::milliseconds(timeout_ms);

    while (clock::now() < deadline && !impl_->stop.load()) {
        // Chrome CDP HTTP API에 TCP 연결 시도
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) break;

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_port        = htons(static_cast<uint16_t>(cdp_port));
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            // HTTP GET /json — 열려 있는 탭(page) 목록 반환
            std::string req = "GET /json HTTP/1.1\r\nHost: localhost:" +
                              std::to_string(cdp_port) + "\r\nConnection: close\r\n\r\n";
            if (send(fd, req.data(), req.size(), 0) > 0) {
                // 응답 전체 읽기
                std::string resp;
                resp.reserve(4096);
                char buf[4096];
                ssize_t n;
                while ((n = recv(fd, buf, sizeof(buf), 0)) > 0)
                    resp.append(buf, static_cast<size_t>(n));

                // HTTP 본문 추출 (\r\n\r\n 이후)
                size_t body_pos = resp.find("\r\n\r\n");
                if (body_pos != std::string::npos) {
                    std::string body = resp.substr(body_pos + 4);
                    auto json = nlohmann::json::parse(body, nullptr, /*allow_exceptions=*/false);
                    if (!json.is_discarded() && json.is_array()) {
                        for (auto& tab : json) {
                            // "page" 타입 탭의 WebSocket URL 반환
                            if (tab.value("type", "") == "page" &&
                                tab.contains("webSocketDebuggerUrl")) {
                                close(fd);
                                return tab["webSocketDebuggerUrl"].get<std::string>();
                            }
                        }
                    }
                }
            }
        }
        close(fd);

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    throw std::runtime_error("CDP WebSocket URL not found (port=" +
                             std::to_string(cdp_port) + ")");
}

// ── CDP WebSocket 연결 ────────────────────────────────────────────────────

int GuacWebClient::connect_cdp_ws(const std::string& ws_url) {
    // URL 파싱: ws://HOST:PORT/PATH
    if (ws_url.substr(0, 5) != "ws://") return -1;
    std::string rest = ws_url.substr(5);

    size_t slash_pos = rest.find('/');
    std::string host_port = (slash_pos != std::string::npos)
                                ? rest.substr(0, slash_pos) : rest;
    std::string path = (slash_pos != std::string::npos)
                           ? rest.substr(slash_pos) : "/";

    size_t colon_pos = host_port.find(':');
    std::string host = host_port.substr(0, colon_pos);
    int port = 9222;
    if (colon_pos != std::string::npos)
        port = std::stoi(host_port.substr(colon_pos + 1));

    // TCP 연결
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(static_cast<uint16_t>(port));
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    // RFC 6455 WebSocket 클라이언트 핸드셰이크
    // Sec-WebSocket-Key: /dev/urandom에서 16바이트 읽어 base64 인코딩
    uint8_t key_bytes[16];
    int rfd = open("/dev/urandom", O_RDONLY);
    if (rfd < 0) { close(fd); return -1; }
    ssize_t r = read(rfd, key_bytes, 16);
    close(rfd);
    if (r != 16) { close(fd); return -1; }

    char key_b64[32] = {};
    EVP_EncodeBlock(reinterpret_cast<unsigned char*>(key_b64), key_bytes, 16);
    std::string ws_key(key_b64);

    std::string handshake =
        "GET " + path + " HTTP/1.1\r\n"
        "Host: " + host_port + "\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: " + ws_key + "\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n";

    if (send(fd, handshake.data(), handshake.size(), 0) < 0) {
        close(fd);
        return -1;
    }

    // 101 Switching Protocols 응답 확인
    std::string resp;
    resp.reserve(512);
    char buf[512];
    while (resp.find("\r\n\r\n") == std::string::npos) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) { close(fd); return -1; }
        resp.append(buf, static_cast<size_t>(n));
    }

    if (resp.find("101") == std::string::npos) {
        close(fd);
        return -1;
    }

    return fd;
}

// ── CDP 메시지 송수신 ─────────────────────────────────────────────────────

bool GuacWebClient::cdp_send(int ws_fd, int id, const std::string& method,
                              const std::string& params) {
    // {"id":N,"method":"Method","params":{...}}
    std::string payload = "{\"id\":" + std::to_string(id) +
                          ",\"method\":\"" + method +
                          "\",\"params\":" + params + "}";

    // RFC 6455 §5.3: 클라이언트→서버 프레임은 마스킹 필수
    uint8_t mask[4];
    int rfd = open("/dev/urandom", O_RDONLY);
    if (rfd < 0) return false;
    ssize_t r = read(rfd, mask, 4);
    close(rfd);
    if (r != 4) return false;

    size_t len = payload.size();
    std::vector<uint8_t> frame;
    frame.reserve(len + 10);

    frame.push_back(0x81);  // FIN=1, opcode=0x01 (Text)

    // 페이로드 길이 + MASK 비트(0x80)
    if (len < 126) {
        frame.push_back(0x80 | static_cast<uint8_t>(len));
    } else if (len < 65536) {
        frame.push_back(0x80 | 126);
        frame.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
        frame.push_back(static_cast<uint8_t>(len & 0xFF));
    } else {
        frame.push_back(0x80 | 127);
        for (int i = 7; i >= 0; i--)
            frame.push_back(static_cast<uint8_t>((len >> (i * 8)) & 0xFF));
    }

    // 마스킹 키
    frame.insert(frame.end(), mask, mask + 4);

    // 마스킹된 페이로드
    for (size_t i = 0; i < len; i++)
        frame.push_back(static_cast<uint8_t>(payload[i]) ^ mask[i % 4]);

    return send(ws_fd, frame.data(), frame.size(), 0) > 0;
}

std::string GuacWebClient::cdp_recv(int ws_fd) {
    while (true) {
        uint8_t hdr[2];
        if (recv_exact(ws_fd, hdr, 2) != 2) return "";

        uint8_t opcode     = hdr[0] & 0x0F;
        bool    masked     = (hdr[1] & 0x80) != 0;
        size_t  payload_len = hdr[1] & 0x7F;

        // 확장 페이로드 길이
        if (payload_len == 126) {
            uint8_t ext[2];
            if (recv_exact(ws_fd, ext, 2) != 2) return "";
            payload_len = (static_cast<size_t>(ext[0]) << 8) | ext[1];
        } else if (payload_len == 127) {
            uint8_t ext[8];
            if (recv_exact(ws_fd, ext, 8) != 8) return "";
            payload_len = 0;
            for (int i = 0; i < 8; i++)
                payload_len = (payload_len << 8) | ext[i];
        }

        // 마스킹 키 (서버→클라이언트는 보통 없음)
        uint8_t mask_key[4] = {};
        if (masked && recv_exact(ws_fd, mask_key, 4) != 4) return "";

        // 페이로드 읽기
        std::string payload(payload_len, '\0');
        if (payload_len > 0 &&
            recv_exact(ws_fd, payload.data(), payload_len) !=
                static_cast<ssize_t>(payload_len)) return "";

        if (masked) {
            for (size_t i = 0; i < payload_len; i++)
                payload[i] ^= mask_key[i % 4];
        }

        if (opcode == 8) return "";  // Close 프레임

        // Ping 수신 → Pong 전송 후 다음 프레임으로 계속
        if (opcode == 9) {
            std::vector<uint8_t> pong;
            pong.push_back(0x8A);  // FIN=1, opcode=0x0A (Pong)
            pong.push_back(static_cast<uint8_t>(payload.size()));
            pong.insert(pong.end(), payload.begin(), payload.end());
            send(ws_fd, pong.data(), pong.size(), 0);
            continue;
        }

        return payload;
    }
}

// ── run_event_loop ────────────────────────────────────────────────────────

void GuacWebClient::run_event_loop(const std::string& url, int width, int height) {
    // 1. Chrome headless 프로세스 실행
    pid_t pid = fork_chromium(impl_->cdp_port, width, height);
    if (pid < 0) return;
    impl_->chrome_pid = pid;

    // 2. CDP WebSocket URL 획득 (Chrome 시작 완료까지 최대 5초 대기)
    std::string ws_url;
    try {
        ws_url = get_cdp_ws_url(impl_->cdp_port);
    } catch (const std::exception&) {
        return;
    }

    if (impl_->stop.load()) return;

    // 3. CDP WebSocket 연결
    int ws_fd = connect_cdp_ws(ws_url);
    if (ws_fd < 0) return;
    impl_->cdp_ws_fd = ws_fd;

    // 4. Page 이벤트 구독 활성화
    //    Page.enable 없이는 Page.loadEventFired 이벤트가 오지 않는다.
    cdp_send(ws_fd, 1, "Page.enable");
    cdp_recv(ws_fd);  // {"id":1,"result":{}} 응답 소비

    if (impl_->stop.load()) { close(ws_fd); impl_->cdp_ws_fd = -1; return; }

    // 5. 페이지 로드 시작
    std::string nav_params = "{\"url\":\"" + url + "\"}";
    cdp_send(ws_fd, 2, "Page.navigate", nav_params);

    // 6. Page.loadEventFired 이벤트 대기
    //    frameNavigated, domContentEventFired 등 여러 이벤트가 먼저 도착하므로 루프.
    while (!impl_->stop.load()) {
        std::string msg = cdp_recv(ws_fd);
        if (msg.empty()) break;

        auto json = nlohmann::json::parse(msg, nullptr, /*allow_exceptions=*/false);
        if (json.is_discarded()) continue;

        std::string method = json.value("method", "");
        if (method == "Page.loadEventFired") {
            // 캔버스 초기화 명령어 전달 (GuacVncClient와 동일한 패턴)
            GuacInstruction size_instr{"size",
                {std::to_string(width), std::to_string(height)}};
            callback_(size_instr);
            connected_ = true;
            break;
        }
    }

    // Phase 13-B/C: captureScreenshot 루프 + 입력 이벤트 처리
    //
    // ID 할당: 1=Page.enable, 2=Page.navigate, 3+=입력이벤트/captureScreenshot
    // 반복마다: ① 입력 큐 drain → ② 스크린샷 캡처
    // CDP 이벤트(id 필드 없음)는 무시하고 해당 id의 응답을 대기한다.
    int next_id = 3;
    bool streaming = true;

    while (!impl_->stop.load() && streaming) {
        // Phase 13-C: 입력 이벤트를 먼저 처리한다.
        // drain_input_queue가 CDP 전송 + 응답 소비를 담당한다.
        if (!drain_input_queue(ws_fd, next_id)) break;

        // 스크린샷 캡처
        cdp_send(ws_fd, next_id, "Page.captureScreenshot",
                 "{\"format\":\"jpeg\",\"quality\":80}");

        // 이 요청에 대한 응답(id 매칭)을 찾을 때까지 메시지를 소비한다.
        bool got_response = false;
        while (!impl_->stop.load() && !got_response) {
            std::string msg = cdp_recv(ws_fd);
            if (msg.empty()) { streaming = false; break; }

            auto json = nlohmann::json::parse(msg, nullptr, /*allow_exceptions=*/false);
            if (json.is_discarded()) continue;

            // CDP 이벤트는 "method" 필드만 있고 "id" 필드가 없다.
            // CDP 응답은 "id" 필드와 "result" 또는 "error" 필드를 가진다.
            if (!json.contains("id")) continue;  // 이벤트 → 무시

            if (json["id"].get<int>() == next_id) {
                got_response = true;
                if (json.contains("result") && json["result"].contains("data")) {
                    std::string data = json["result"]["data"].get<std::string>();
                    flush_delta(data, impl_->next_stream_id++);
                }
            }
        }
        ++next_id;
    }

    close(ws_fd);
    impl_->cdp_ws_fd = -1;
    connected_ = false;
}

// ── drain_input_queue ─────────────────────────────────────────────────────

bool GuacWebClient::drain_input_queue(int ws_fd, int& next_id) {
    std::unique_lock<std::mutex> lk(impl_->input_mutex);

    while (!impl_->input_queue.empty()) {
        InputEvent event = std::move(impl_->input_queue.front());
        impl_->input_queue.pop();
        lk.unlock();

        int cur_id = next_id++;
        cdp_send(ws_fd, cur_id, event.method, event.params);

        // 입력 이벤트 응답 소비 ({"id": N, "result": {}})
        // 이벤트(method 필드만 있음)는 무시하고 해당 id의 응답만 매칭한다.
        while (!impl_->stop.load()) {
            std::string msg = cdp_recv(ws_fd);
            if (msg.empty()) return false;  // 연결 끊어짐

            auto json = nlohmann::json::parse(msg, nullptr, /*allow_exceptions=*/false);
            if (!json.is_discarded() && json.contains("id") &&
                json["id"].get<int>() == cur_id) {
                break;  // 응답 수신 완료
            }
            // id 없으면 이벤트 → 무시하고 계속 대기
        }

        lk.lock();
    }
    return true;
}

// ── make_key_params ───────────────────────────────────────────────────────

// X11 keysym → CDP Input.dispatchKeyEvent params JSON
// 변환 대상: ASCII printable + 주요 특수키 + F1-F12 + 수정자 키
std::string GuacWebClient::make_key_params(int keysym, bool pressed) {
    const std::string type = pressed ? "keyDown" : "keyUp";
    std::string key;
    std::string text;
    int vk = 0;

    if (keysym >= 0x20 && keysym <= 0x7E) {
        // ASCII printable: space(0x20) ~ tilde(0x7E)
        // Windows VK와 ASCII 코드가 같은 범위 (대소문자 포함)
        key = std::string(1, static_cast<char>(keysym));
        if (pressed) text = key;
        vk = keysym;
    } else {
        switch (keysym) {
            case 0xFF08: key = "Backspace";  vk = 8;   break;
            case 0xFF09: key = "Tab";        vk = 9;   break;
            case 0xFF0D: key = "Enter"; vk = 13;
                         if (pressed) text = "\r";
                         break;
            case 0xFF1B: key = "Escape";     vk = 27;  break;
            case 0xFF50: key = "Home";       vk = 36;  break;
            case 0xFF51: key = "ArrowLeft";  vk = 37;  break;
            case 0xFF52: key = "ArrowUp";    vk = 38;  break;
            case 0xFF53: key = "ArrowRight"; vk = 39;  break;
            case 0xFF54: key = "ArrowDown";  vk = 40;  break;
            case 0xFF55: key = "PageUp";     vk = 33;  break;
            case 0xFF56: key = "PageDown";   vk = 34;  break;
            case 0xFF57: key = "End";        vk = 35;  break;
            case 0xFF63: key = "Insert";     vk = 45;  break;
            case 0xFFFF: key = "Delete";     vk = 46;  break;
            // F1-F12 (0xFFBE=F1 … 0xFFC9=F12)
            case 0xFFBE: key = "F1";  vk = 112; break;
            case 0xFFBF: key = "F2";  vk = 113; break;
            case 0xFFC0: key = "F3";  vk = 114; break;
            case 0xFFC1: key = "F4";  vk = 115; break;
            case 0xFFC2: key = "F5";  vk = 116; break;
            case 0xFFC3: key = "F6";  vk = 117; break;
            case 0xFFC4: key = "F7";  vk = 118; break;
            case 0xFFC5: key = "F8";  vk = 119; break;
            case 0xFFC6: key = "F9";  vk = 120; break;
            case 0xFFC7: key = "F10"; vk = 121; break;
            case 0xFFC8: key = "F11"; vk = 122; break;
            case 0xFFC9: key = "F12"; vk = 123; break;
            // 수정자 키 (L/R 구분 없이 같은 DOM key)
            case 0xFFE1: case 0xFFE2: key = "Shift";   vk = 16; break;
            case 0xFFE3: case 0xFFE4: key = "Control"; vk = 17; break;
            case 0xFFE9: case 0xFFEA: key = "Alt";     vk = 18; break;
            case 0xFFE7: case 0xFFE8: key = "Meta";    vk = 91; break;
            default:                  key = "Unidentified"; break;
        }
    }

    // JSON 직접 조합 (nlohmann/json을 쓰면 불필요한 의존성)
    // key / text 필드는 DOM key name이므로 특수문자 이스케이프가 필요한 경우:
    // printable ASCII에서 " \ 만 처리, \r (Enter text) 처리
    auto escape = [](const std::string& s) {
        std::string out;
        for (char c : s) {
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\r': out += "\\r";  break;
                default:   out += c;
            }
        }
        return out;
    };

    std::string params = "{\"type\":\"" + type + "\",\"key\":\"" + escape(key) + "\"";
    if (vk > 0)
        params += ",\"windowsVirtualKeyCode\":" + std::to_string(vk);
    if (!text.empty())
        params += ",\"text\":\"" + escape(text) + "\"";
    params += "}";
    return params;
}

// ── send_mouse / send_key ─────────────────────────────────────────────────

void GuacWebClient::send_mouse(int x, int y, int button_mask) {
    int prev = impl_->prev_button_mask;
    impl_->prev_button_mask = button_mask;

    // Guacamole → CDP buttons bitmask 변환
    // Guacamole: bit0=left, bit1=middle, bit2=right
    // CDP (W3C): bit0(1)=left, bit1(2)=right, bit2(4)=middle
    int cdp_buttons = 0;
    if (button_mask & 1) cdp_buttons |= 1;  // left
    if (button_mask & 2) cdp_buttons |= 4;  // middle
    if (button_mask & 4) cdp_buttons |= 2;  // right

    // CDP mouseEvent params 생성 헬퍼
    auto mouse_params = [&](const std::string& type, const std::string& button,
                             int click_count = 0) {
        std::string p = "{\"type\":\"" + type + "\""
                        ",\"x\":" + std::to_string(x) +
                        ",\"y\":" + std::to_string(y) +
                        ",\"button\":\"" + button + "\"" +
                        ",\"buttons\":" + std::to_string(cdp_buttons);
        if (click_count > 0)
            p += ",\"clickCount\":" + std::to_string(click_count);
        return p + "}";
    };

    auto push = [&](const std::string& method, const std::string& params) {
        std::lock_guard<std::mutex> lk(impl_->input_mutex);
        impl_->input_queue.push({method, params});
    };

    // 스크롤 처리 (bit 3/4는 순간적 — prev와 비교하지 않고 항상 전송)
    if (button_mask & 8) {  // 스크롤 위
        push("Input.dispatchMouseEvent",
             "{\"type\":\"mouseWheel\""
             ",\"x\":" + std::to_string(x) +
             ",\"y\":" + std::to_string(y) +
             ",\"deltaX\":0,\"deltaY\":-120}");
    }
    if (button_mask & 16) {  // 스크롤 아래
        push("Input.dispatchMouseEvent",
             "{\"type\":\"mouseWheel\""
             ",\"x\":" + std::to_string(x) +
             ",\"y\":" + std::to_string(y) +
             ",\"deltaX\":0,\"deltaY\":120}");
    }

    // 버튼 변화 감지 (bit 0-2만 비교)
    int newly_pressed  = (button_mask & ~prev) & 0x07;
    int newly_released = (~button_mask & prev) & 0x07;

    // mousePressed: 새로 눌린 버튼
    if (newly_pressed & 1)
        push("Input.dispatchMouseEvent", mouse_params("mousePressed", "left", 1));
    if (newly_pressed & 2)
        push("Input.dispatchMouseEvent", mouse_params("mousePressed", "middle", 1));
    if (newly_pressed & 4)
        push("Input.dispatchMouseEvent", mouse_params("mousePressed", "right", 1));

    // mouseReleased: 해제된 버튼
    if (newly_released & 1)
        push("Input.dispatchMouseEvent", mouse_params("mouseReleased", "left", 1));
    if (newly_released & 2)
        push("Input.dispatchMouseEvent", mouse_params("mouseReleased", "middle", 1));
    if (newly_released & 4)
        push("Input.dispatchMouseEvent", mouse_params("mouseReleased", "right", 1));

    // mouseMoved: 항상 전송해 위치를 업데이트한다 (버튼 상태 포함)
    push("Input.dispatchMouseEvent", mouse_params("mouseMoved", "none"));
}

void GuacWebClient::send_key(int keysym, bool pressed) {
    std::string params = make_key_params(keysym, pressed);
    std::lock_guard<std::mutex> lk(impl_->input_mutex);
    impl_->input_queue.push({"Input.dispatchKeyEvent", std::move(params)});
}

// ── flush_delta 헬퍼 ──────────────────────────────────────────────────────

// base64 문자열 → raw bytes (OpenSSL EVP_DecodeBlock)
static std::vector<uint8_t> base64_decode(const std::string& b64) {
    if (b64.empty()) return {};
    std::vector<uint8_t> buf(b64.size());
    int len = EVP_DecodeBlock(
        buf.data(),
        reinterpret_cast<const unsigned char*>(b64.data()),
        static_cast<int>(b64.size()));
    if (len < 0) return {};
    // base64 패딩('=')으로 인해 끝에 최대 2바이트 0이 추가됨 — 제거
    size_t padding = 0;
    if (b64.size() >= 1 && b64.back()             == '=') padding++;
    if (b64.size() >= 2 && b64[b64.size() - 2]   == '=') padding++;
    buf.resize(static_cast<size_t>(len) - padding);
    return buf;
}

// raw JPEG bytes → 특정 rect의 base64 JPEG 문자열 생성
// rect가 (0,0,w,h)이면 전체 원본 b64_jpeg를 그대로 반환 (재인코딩 생략)
static std::string encode_rect_jpeg(
        const std::string& b64_full,
        const uint8_t* pixels, int full_w,
        int rx, int ry, int rw, int rh) {

    if (rx == 0 && ry == 0 && rw == full_w) {
        // 전체 프레임 — 원본 base64 그대로 사용
        return b64_full;
    }

    // dirty rect 픽셀 추출 (RGB24)
    std::vector<uint8_t> sub;
    sub.reserve(static_cast<size_t>(rw) * rh * 3);
    for (int row = ry; row < ry + rh; row++) {
        const uint8_t* src = pixels + (row * full_w + rx) * 3;
        sub.insert(sub.end(), src, src + rw * 3);
    }

    // stb_image_write로 JPEG 인코딩
    std::vector<uint8_t> jpeg_out;
    stbi_write_jpg_to_func(
        [](void* ctx, void* data, int size) {
            auto* out = static_cast<std::vector<uint8_t>*>(ctx);
            out->insert(out->end(),
                        static_cast<uint8_t*>(data),
                        static_cast<uint8_t*>(data) + size);
        },
        &jpeg_out, rw, rh, 3, sub.data(), 85  // quality 85
    );

    if (jpeg_out.empty()) return b64_full;  // 인코딩 실패 → fallback

    // base64 인코딩 (OpenSSL EVP_EncodeBlock)
    size_t b64_len = ((jpeg_out.size() + 2) / 3) * 4 + 1;
    std::string b64(b64_len, '\0');
    int len = EVP_EncodeBlock(
        reinterpret_cast<unsigned char*>(b64.data()),
        jpeg_out.data(), static_cast<int>(jpeg_out.size()));
    b64.resize(static_cast<size_t>(len));
    return b64;
}

// Guacamole img/blob/end 전송 헬퍼
static void send_guac_image(
        const std::function<void(const GuacInstruction&)>& callback,
        int stream_id, int x, int y, const std::string& b64_jpeg) {

    callback(GuacInstruction{"img",
        {std::to_string(stream_id), "over", "0", "image/jpeg",
         std::to_string(x), std::to_string(y)}});

    static const size_t CHUNK = 8192;
    for (size_t off = 0; off < b64_jpeg.size(); off += CHUNK) {
        callback(GuacInstruction{"blob",
            {std::to_string(stream_id), b64_jpeg.substr(off, CHUNK)}});
    }
    callback(GuacInstruction{"end", {std::to_string(stream_id)}});
}

// ── flush_delta ───────────────────────────────────────────────────────────

void GuacWebClient::flush_delta(const std::string& b64_jpeg, int stream_id) {
    // 1. base64 decode → raw JPEG bytes
    auto jpeg_bytes = base64_decode(b64_jpeg);
    if (jpeg_bytes.empty()) return;

    // 2. JPEG decode → RGB24 픽셀 (stb_image)
    int w = 0, h = 0, comp = 0;
    uint8_t* pixels = stbi_load_from_memory(
        jpeg_bytes.data(), static_cast<int>(jpeg_bytes.size()),
        &w, &h, &comp, 3  // force RGB24
    );
    if (!pixels) {
        // JPEG 디코딩 실패 → 전체 프레임 전송 (fallback)
        send_guac_image(callback_, stream_id, 0, 0, b64_jpeg);
        return;
    }

    const size_t pixel_bytes = static_cast<size_t>(w) * h * 3;

    // 3. 이전 프레임과 픽셀 단위 비교 → dirty bounding rect 계산
    bool has_prev = (impl_->prev_pixels.size() == pixel_bytes &&
                     impl_->frame_width == w && impl_->frame_height == h);

    // dirty rect: (rx, ry, rw, rh)
    int rx = 0, ry = 0, rw = w, rh = h;  // 기본: 전체 프레임

    if (has_prev) {
        int min_row = -1, max_row = -1;
        int min_col = w,  max_col = -1;

        for (int row = 0; row < h; row++) {
            const uint8_t* prev_row = impl_->prev_pixels.data() + row * w * 3;
            const uint8_t* curr_row = pixels + row * w * 3;

            // 행 전체를 memcmp로 빠르게 비교
            if (memcmp(prev_row, curr_row, static_cast<size_t>(w) * 3) == 0) continue;

            if (min_row == -1) min_row = row;
            max_row = row;

            // 변화 있는 행에서 열 단위 스캔 (dirty 열 범위 계산)
            for (int col = 0; col < w; col++) {
                const uint8_t* p = prev_row + col * 3;
                const uint8_t* c = curr_row + col * 3;
                if (p[0] != c[0] || p[1] != c[1] || p[2] != c[2]) {
                    min_col = std::min(min_col, col);
                    max_col = std::max(max_col, col);
                }
            }
        }

        if (min_row == -1) {
            // 변화 없음 — 전송 스킵
            stbi_image_free(pixels);
            return;
        }

        rx = min_col;
        ry = min_row;
        rw = max_col - min_col + 1;
        rh = max_row - min_row + 1;
    }

    // 4. 현재 픽셀을 이전 프레임으로 저장 (다음 비교용)
    impl_->prev_pixels.assign(pixels, pixels + pixel_bytes);
    impl_->frame_width  = w;
    impl_->frame_height = h;

    // 5. dirty rect vs 전체 비교 → 75% 이상이면 원본 전체 전송
    //    부분 인코딩 오버헤드보다 원본 전송이 더 효율적인 임계값
    const bool use_full = (rw * rh * 4 >= w * h * 3);  // dirty >= 75%
    if (use_full) { rx = ry = 0; rw = w; rh = h; }

    // 6. dirty rect JPEG 생성 (재인코딩 또는 원본 재사용)
    std::string b64_out = encode_rect_jpeg(b64_jpeg, pixels, w, rx, ry, rw, rh);
    stbi_image_free(pixels);

    // 7. Guacamole img/blob/end 전송
    send_guac_image(callback_, stream_id, rx, ry, b64_out);
}

} // namespace proxy
