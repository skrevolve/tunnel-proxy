/**
 * @file guac_web.cpp
 * @brief Phase 13-A — Chrome headless fork/exec + CDP WebSocket 클라이언트 + 페이지 로드
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
 * ── Chrome 종료 전략 ──────────────────────────────────────────────────────
 *
 *   disconnect() → SIGTERM 전송 → 500ms waitpid → 아직 살아있으면 SIGKILL
 *   좀비 프로세스 방지를 위해 waitpid를 반드시 호출한다.
 */

#include "core/guac_web.h"

#include <nlohmann/json.hpp>
#include <openssl/evp.h>  // EVP_EncodeBlock (base64)

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
#include <stdexcept>
#include <cstring>
#include <vector>
#include <string>

namespace proxy {

// ── GuacWebClient::Impl ───────────────────────────────────────────────────

struct GuacWebClient::Impl {
    std::atomic<bool> stop{false};
    pid_t             chrome_pid{-1};    // fork된 Chrome 프로세스 PID
    int               cdp_ws_fd{-1};    // CDP WebSocket TCP fd
    int               cdp_port{9222};   // Chrome CDP 포트
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

    // TODO(Phase 13-B): Page.captureScreenshot 루프 추가
    // while (!impl_->stop.load()) {
    //     cdp_send(ws_fd, next_id++, "Page.captureScreenshot",
    //              "{\"format\":\"jpeg\",\"quality\":80}");
    //     std::string resp = cdp_recv(ws_fd);
    //     // resp["result"]["data"] → base64 JPEG → GuacInstruction img/blob/end
    // }

    close(ws_fd);
    impl_->cdp_ws_fd = -1;
    connected_ = false;
}

} // namespace proxy
