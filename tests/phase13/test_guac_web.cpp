/**
 * @file test_guac_web.cpp
 * @brief Phase 13-E — GuacWebClient + GuacWebSocketGateway E2E 테스트 (MockCdpServer)
 *
 * ── MockCdpServer 구조 ───────────────────────────────────────────────────────
 *
 *   GuacWebClient가 여는 두 종류의 TCP 연결을 단일 포트에서 처리한다:
 *
 *   1. HTTP GET /json  — get_cdp_ws_url()의 CDP 타겟 목록 요청
 *      응답: [{"type":"page","webSocketDebuggerUrl":"ws://127.0.0.1:PORT/cdp"}]
 *
 *   2. WebSocket 업그레이드 — connect_cdp_ws()의 CDP WS 연결
 *      핸드셰이크 후 CDP JSON 메시지를 교환한다:
 *        Page.enable     → {"id":N,"result":{}}
 *        Page.navigate   → {"id":N,"result":{}} + Page.loadEventFired 이벤트
 *        Page.captureScreenshot → {"id":N,"result":{"data":"<b64_jpeg>"}}
 *        Input.*         → {"id":N,"result":{}}
 *
 * ── fork_chromium 생략 조건 ──────────────────────────────────────────────────
 *
 *   GuacWebClient::fork_chromium()은 cdp_port에 이미 리스너가 있으면 Chrome fork를
 *   생략하고 0을 반환한다. MockCdpServer를 먼저 시작한 뒤 connect()를 호출하면
 *   Chromium 없이 전체 파이프라인을 테스트할 수 있다.
 *
 * ── 테스트 시나리오 ─────────────────────────────────────────────────────────
 *
 *   [GuacWebClient 생명주기]
 *   S1. disconnect() before connect — 크래시 없음
 *   S2. send_mouse() / send_key() before connect — 크래시 없음
 *   S3. connect() → MockCdpServer → is_connected() == true
 *   S4. connect() → disconnect() → is_connected() == false
 *
 *   [명령어 스트리밍]
 *   S5. loadEventFired 후 size 명령어 수신 (width/height 확인)
 *   S6. captureScreenshot 응답 → img / blob / end 시퀀스 수신
 *   S7. 동일 프레임 2회 → 두 번째는 blob 전송 없음 (delta skip)
 *   S8. 다른 프레임 2회 → 두 번 모두 end 명령어 수신
 *
 *   [입력 주입]
 *   S9.  send_mouse() → MockCdpServer에서 Input.dispatchMouseEvent 수신
 *   S10. send_key()   → MockCdpServer에서 Input.dispatchKeyEvent 수신
 *
 *   [GuacWebSocketGateway]
 *   S11. start()/stop() 생명주기 — is_running() 변화
 *   S12. WebSocket 클라이언트가 connect,web,...; 전송 — 크래시 없이 처리
 *   S13. 미지원 프로토콜 → error 명령어 응답 수신
 */

#include "core/guac_web.h"
#include "core/guac_websocket.h"
#include "core/guac_parser.h"

#include <gtest/gtest.h>

// stb — 선언만 (구현은 guac_web.cpp 번역 단위에 있음)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#include <stb_image_write.h>
#pragma GCC diagnostic pop

#include <openssl/evp.h>
#include <openssl/sha.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <future>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

using namespace proxy;

// ── 공통 헬퍼: 사용 가능한 TCP 포트 ─────────────────────────────────────────

static uint16_t get_free_port() {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = 0;
    bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    socklen_t len = sizeof(addr);
    getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    uint16_t port = ntohs(addr.sin_port);
    close(fd);
    return port;
}

// ── make_solid_b64_jpeg: 단색 JPEG → base64 문자열 ──────────────────────────
//
// delta 테스트에서 "같은 프레임"과 "다른 프레임"을 명확히 구분하기 위해 사용한다.
// stbi_write_jpg_to_func (구현은 guac_web.cpp) + EVP_EncodeBlock (OpenSSL).

static std::string make_solid_b64_jpeg(int w, int h, uint8_t r, uint8_t g, uint8_t b) {
    std::vector<uint8_t> pixels(static_cast<size_t>(w * h * 3));
    for (int i = 0; i < w * h; i++) {
        pixels[static_cast<size_t>(i * 3)]     = r;
        pixels[static_cast<size_t>(i * 3 + 1)] = g;
        pixels[static_cast<size_t>(i * 3 + 2)] = b;
    }

    std::vector<uint8_t> jpeg;
    stbi_write_jpg_to_func(
        [](void* ctx, void* data, int size) {
            auto* out = static_cast<std::vector<uint8_t>*>(ctx);
            const auto* d = static_cast<const uint8_t*>(data);
            out->insert(out->end(), d, d + size);
        },
        &jpeg, w, h, 3, pixels.data(), 85);

    if (jpeg.empty()) return "";
    size_t b64_max = ((jpeg.size() + 2) / 3) * 4 + 1;
    std::string b64(b64_max, '\0');
    int len = EVP_EncodeBlock(
        reinterpret_cast<unsigned char*>(b64.data()),
        jpeg.data(), static_cast<int>(jpeg.size()));
    b64.resize(static_cast<size_t>(len));
    return b64;
}

// ── InstrCollector: 콜백으로 수신한 GuacInstruction 수집 ────────────────────

struct InstrCollector {
    mutable std::mutex      mtx;
    std::condition_variable cv;
    std::vector<GuacInstruction> received;

    GuacWebClient::InstructionCallback callback() {
        return [this](const GuacInstruction& i) {
            { std::lock_guard<std::mutex> lk(mtx); received.push_back(i); }
            cv.notify_all();
        };
    }

    bool wait_for_opcode(const std::string& op,
                         std::chrono::seconds timeout = std::chrono::seconds(5)) {
        std::unique_lock<std::mutex> lk(mtx);
        return cv.wait_for(lk, timeout, [&] {
            for (const auto& i : received)
                if (i.opcode == op) return true;
            return false;
        });
    }

    int count(const std::string& op) const {
        std::lock_guard<std::mutex> lk(mtx);
        int n = 0;
        for (const auto& i : received)
            if (i.opcode == op) n++;
        return n;
    }

    GuacInstruction first(const std::string& op) const {
        std::lock_guard<std::mutex> lk(mtx);
        for (const auto& i : received)
            if (i.opcode == op) return i;
        return {};
    }
};

// ── MockCdpServer ────────────────────────────────────────────────────────────
//
// GuacWebClient가 열어야 하는 두 종류의 연결을 단일 포트에서 처리한다:
//   1. HTTP GET /json  → CDP 타겟 목록 JSON 반환 후 연결 종료
//   2. WebSocket 업그레이드 → CDP 메시지 루프 진입
//
// 스크린샷 큐:
//   push_screenshot(b64)는 std::future<void>를 반환한다.
//   MockCdpServer가 해당 b64를 captureScreenshot 응답으로 전송하면 future가 resolve된다.
//   큐가 비어있을 때 들어오는 captureScreenshot 요청은 빈 result로 응답한다 (flush_delta 미호출).

class MockCdpServer {
public:
    MockCdpServer() {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        int opt = 1;
        setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port        = 0;
        bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        socklen_t len = sizeof(addr);
        getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
        listen(listen_fd_, 10);
        thread_ = std::thread(&MockCdpServer::run, this);
    }

    ~MockCdpServer() {
        stop_ = true;
        // 활성 WebSocket fd를 먼저 닫아 handle_cdp_session recv 블로킹 해제
        int ws = ws_fd_.exchange(-1);
        if (ws >= 0) { shutdown(ws, SHUT_RDWR); close(ws); }
        if (listen_fd_ >= 0) { shutdown(listen_fd_, SHUT_RDWR); close(listen_fd_); listen_fd_ = -1; }
        if (thread_.joinable()) thread_.join();
    }

    uint16_t port() const { return port_; }

    // b64 JPEG를 다음 captureScreenshot 응답으로 예약한다.
    // 반환된 future는 해당 응답이 전송된 시점에 resolve된다.
    std::future<void> push_screenshot(const std::string& b64) {
        std::promise<void> p;
        auto f = p.get_future();
        std::lock_guard<std::mutex> lk(mtx_);
        queue_.push({b64, std::move(p)});
        return f;
    }

    // 특정 CDP 메서드가 수신될 때까지 대기한다.
    bool wait_for_method(const std::string& method,
                         std::chrono::seconds timeout = std::chrono::seconds(5)) {
        std::unique_lock<std::mutex> lk(mtx_);
        return cv_.wait_for(lk, timeout, [&] {
            for (const auto& m : received_methods_)
                if (m == method) return true;
            return false;
        });
    }

    // 수신된 메서드 이름 목록 (순서 보존)
    std::vector<std::string> received_methods() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return received_methods_;
    }

    // 특정 메서드의 마지막 params JSON 반환
    std::string last_params(const std::string& method) const {
        std::lock_guard<std::mutex> lk(mtx_);
        for (int i = static_cast<int>(received_methods_.size()) - 1; i >= 0; i--) {
            if (received_methods_[static_cast<size_t>(i)] == method)
                return received_params_[static_cast<size_t>(i)];
        }
        return "";
    }

private:
    struct ScreenshotEntry {
        std::string       b64;
        std::promise<void> promise;
    };

    int               listen_fd_{-1};
    uint16_t          port_{0};
    std::thread       thread_;
    std::atomic<bool> stop_{false};
    std::atomic<int>  ws_fd_{-1};

    mutable std::mutex      mtx_;
    std::condition_variable cv_;
    std::queue<ScreenshotEntry> queue_;
    std::vector<std::string>    received_methods_;
    std::vector<std::string>    received_params_;

    // ── 수신 헬퍼 ────────────────────────────────────────────────────────────

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

    static std::string read_http_request(int fd) {
        std::string req;
        req.reserve(1024);
        char c;
        while (req.size() < 8192) {
            if (::recv(fd, &c, 1, 0) <= 0) break;
            req += c;
            if (req.size() >= 4 && req.compare(req.size() - 4, 4, "\r\n\r\n") == 0) break;
        }
        return req;
    }

    // ── WebSocket 프레임 헬퍼 ─────────────────────────────────────────────────

    // 클라이언트→서버 프레임 읽기 (MASKED)
    std::string recv_ws_frame(int fd) {
        uint8_t hdr[2];
        if (recv_exact(fd, hdr, 2) != 2) return "";

        uint8_t opcode = hdr[0] & 0x0F;
        if (opcode == 8) return "";  // Close

        bool   masked      = (hdr[1] & 0x80) != 0;
        size_t payload_len = hdr[1] & 0x7F;

        if (payload_len == 126) {
            uint8_t ext[2];
            if (recv_exact(fd, ext, 2) != 2) return "";
            payload_len = (static_cast<size_t>(ext[0]) << 8) | ext[1];
        } else if (payload_len == 127) {
            uint8_t ext[8];
            if (recv_exact(fd, ext, 8) != 8) return "";
            payload_len = 0;
            for (int i = 0; i < 8; i++) payload_len = (payload_len << 8) | ext[i];
        }

        uint8_t mask[4] = {};
        if (masked && recv_exact(fd, mask, 4) != 4) return "";

        std::string payload(payload_len, '\0');
        if (payload_len > 0 &&
            recv_exact(fd, payload.data(), payload_len) !=
                static_cast<ssize_t>(payload_len)) return "";

        if (masked)
            for (size_t i = 0; i < payload_len; i++) payload[i] ^= mask[i % 4];

        return payload;
    }

    // 서버→클라이언트 프레임 전송 (UNMASKED)
    static void send_ws_text(int fd, const std::string& text) {
        std::vector<uint8_t> frame;
        frame.push_back(0x81);  // FIN + Text opcode
        size_t len = text.size();
        if (len < 126) {
            frame.push_back(static_cast<uint8_t>(len));
        } else if (len < 65536) {
            frame.push_back(126);
            frame.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
            frame.push_back(static_cast<uint8_t>(len & 0xFF));
        } else {
            frame.push_back(127);
            for (int i = 7; i >= 0; i--)
                frame.push_back(static_cast<uint8_t>((len >> (i * 8)) & 0xFF));
        }
        frame.insert(frame.end(), text.begin(), text.end());
        ::write(fd, frame.data(), frame.size());
    }

    // ── WebSocket 핸드셰이크 ──────────────────────────────────────────────────

    static bool do_ws_handshake(int fd, const std::string& req) {
        const std::string KEY_HEADER = "Sec-WebSocket-Key:";
        size_t pos = req.find(KEY_HEADER);
        if (pos == std::string::npos) return false;
        pos += KEY_HEADER.size();
        while (pos < req.size() && req[pos] == ' ') pos++;
        size_t end = req.find("\r\n", pos);
        if (end == std::string::npos) return false;
        std::string key = req.substr(pos, end - pos);

        const std::string GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        std::string combined = key + GUID;
        uint8_t sha1[20];
        unsigned int sha1_len = 20;
        EVP_MD_CTX* ctx = EVP_MD_CTX_new();
        EVP_DigestInit_ex(ctx, EVP_sha1(), nullptr);
        EVP_DigestUpdate(ctx, combined.data(), combined.size());
        EVP_DigestFinal_ex(ctx, sha1, &sha1_len);
        EVP_MD_CTX_free(ctx);

        char b64[64];
        int b64_len = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(b64), sha1, 20);
        std::string accept(b64, static_cast<size_t>(b64_len));

        std::string resp =
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
        return ::write(fd, resp.data(), resp.size()) > 0;
    }

    // ── CDP 세션 루프 ─────────────────────────────────────────────────────────

    void handle_cdp_session(int fd) {
        ws_fd_.store(fd);

        while (!stop_.load()) {
            std::string frame = recv_ws_frame(fd);
            if (frame.empty()) break;

            // JSON 파싱 (간단한 key 추출 — nlohmann 없이)
            auto extract = [&](const std::string& key) -> std::string {
                std::string pat = "\"" + key + "\":";
                size_t p = frame.find(pat);
                if (p == std::string::npos) return "";
                p += pat.size();
                while (p < frame.size() && frame[p] == ' ') p++;
                if (p >= frame.size()) return "";
                if (frame[p] == '"') {
                    size_t e = frame.find('"', p + 1);
                    if (e == std::string::npos) return "";
                    return frame.substr(p + 1, e - p - 1);
                }
                // 숫자
                size_t e = frame.find_first_of(",}", p);
                return frame.substr(p, e == std::string::npos ? std::string::npos : e - p);
            };

            int         id     = 0;
            std::string id_str = extract("id");
            if (!id_str.empty()) id = std::stoi(id_str);

            std::string method = extract("method");

            // params 전체 추출 (단순 brace-match)
            std::string params = "{}";
            {
                size_t pp = frame.find("\"params\":");
                if (pp != std::string::npos) {
                    pp += 9;
                    while (pp < frame.size() && frame[pp] == ' ') pp++;
                    if (pp < frame.size() && frame[pp] == '{') {
                        int depth = 0;
                        size_t start = pp;
                        for (size_t i = pp; i < frame.size(); i++) {
                            if (frame[i] == '{') depth++;
                            else if (frame[i] == '}') { depth--; if (depth == 0) { params = frame.substr(start, i - start + 1); break; } }
                        }
                    }
                }
            }

            // 수신 기록
            {
                std::lock_guard<std::mutex> lk(mtx_);
                received_methods_.push_back(method);
                received_params_.push_back(params);
            }
            cv_.notify_all();

            std::string id_s = std::to_string(id);

            if (method == "Page.enable") {
                send_ws_text(fd, "{\"id\":" + id_s + ",\"result\":{}}");

            } else if (method == "Page.navigate") {
                send_ws_text(fd, "{\"id\":" + id_s + ",\"result\":{\"frameId\":\"1\"}}");
                // loadEventFired 이벤트를 즉시 전송해 GuacWebClient가 connected_=true로 전환하게 한다
                send_ws_text(fd, "{\"method\":\"Page.loadEventFired\","
                                 "\"params\":{\"timestamp\":1.0}}");

            } else if (method == "Page.captureScreenshot") {
                // 큐에서 다음 스크린샷을 꺼낸다
                std::string b64;
                std::promise<void> prom;
                bool has_screenshot = false;
                {
                    std::lock_guard<std::mutex> lk(mtx_);
                    if (!queue_.empty()) {
                        b64  = std::move(queue_.front().b64);
                        prom = std::move(queue_.front().promise);
                        queue_.pop();
                        has_screenshot = true;
                    }
                }
                if (has_screenshot) {
                    send_ws_text(fd, "{\"id\":" + id_s +
                                     ",\"result\":{\"data\":\"" + b64 + "\"}}");
                    prom.set_value();  // future resolve
                } else {
                    // 큐 비어있음 → 빈 result (flush_delta 미호출)
                    send_ws_text(fd, "{\"id\":" + id_s + ",\"result\":{}}");
                }

            } else if (method == "Input.dispatchMouseEvent" ||
                       method == "Input.dispatchKeyEvent") {
                send_ws_text(fd, "{\"id\":" + id_s + ",\"result\":{}}");

            } else if (!method.empty()) {
                // 미지원 메서드도 빈 result로 응답 (루프 유지)
                send_ws_text(fd, "{\"id\":" + id_s + ",\"result\":{}}");
            }
        }

        ws_fd_.store(-1);
    }

    // ── 연결 처리 ────────────────────────────────────────────────────────────

    void handle_connection(int fd) {
        std::string req = read_http_request(fd);
        if (req.empty()) { close(fd); return; }

        if (req.find("GET /json") != std::string::npos) {
            // HTTP GET /json → CDP 타겟 목록 반환
            std::string body =
                "[{\"type\":\"page\","
                "\"webSocketDebuggerUrl\":\"ws://127.0.0.1:" +
                std::to_string(port_) + "/cdp\"}]";
            std::string resp =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/json\r\n"
                "Content-Length: " + std::to_string(body.size()) + "\r\n"
                "Connection: close\r\n\r\n" + body;
            ::write(fd, resp.data(), resp.size());
            close(fd);
            return;
        }

        if (req.find("Upgrade: websocket") != std::string::npos) {
            if (!do_ws_handshake(fd, req)) { close(fd); return; }
            handle_cdp_session(fd);
            close(fd);
            return;
        }

        close(fd);
    }

    // ── Accept 루프 ──────────────────────────────────────────────────────────

    void run() {
        while (!stop_.load()) {
            sockaddr_in caddr{};
            socklen_t   len = sizeof(caddr);
            int cfd = accept(listen_fd_, reinterpret_cast<sockaddr*>(&caddr), &len);
            if (cfd < 0) {
                if (errno == EINTR || errno == EBADF || errno == EINVAL) break;
                continue;
            }
            handle_connection(cfd);
        }
    }
};

// ── WebSocket 테스트 클라이언트 헬퍼 ─────────────────────────────────────────

static int tcp_connect(uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    addr.sin_port = htons(port);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd); return -1;
    }
    return fd;
}

// WebSocket 업그레이드 후 101 응답 확인
static bool ws_handshake(int fd, uint16_t port) {
    std::string req =
        "GET / HTTP/1.1\r\n"
        "Host: localhost:" + std::to_string(port) + "\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n";
    ::write(fd, req.data(), req.size());

    char buf[1024];
    ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) return false;
    buf[n] = '\0';
    return std::string(buf).find("101") != std::string::npos;
}

// Guacamole 명령어를 WebSocket text 프레임으로 전송 (마스킹)
static void ws_send_guac(int fd, const std::string& text) {
    uint8_t mask[4] = {0x11, 0x22, 0x33, 0x44};
    std::vector<uint8_t> frame;
    frame.push_back(0x81);
    size_t len = text.size();
    if (len < 126) {
        frame.push_back(0x80 | static_cast<uint8_t>(len));
    } else {
        frame.push_back(0x80 | 126);
        frame.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
        frame.push_back(static_cast<uint8_t>(len & 0xFF));
    }
    frame.insert(frame.end(), mask, mask + 4);
    for (size_t i = 0; i < len; i++)
        frame.push_back(static_cast<uint8_t>(text[i]) ^ mask[i % 4]);
    ::write(fd, frame.data(), frame.size());
}

// WebSocket text 프레임 하나 읽기 (언마스크)
static std::string ws_recv_frame(int fd) {
    auto recv_exact = [](int f, void* b, size_t n) -> ssize_t {
        size_t total = 0;
        while (total < n) {
            ssize_t r = recv(f, static_cast<char*>(b) + total, n - total, MSG_WAITALL);
            if (r <= 0) return -1;
            total += static_cast<size_t>(r);
        }
        return static_cast<ssize_t>(total);
    };

    uint8_t hdr[2];
    if (recv_exact(fd, hdr, 2) < 0) return "";
    size_t payload_len = hdr[1] & 0x7F;
    if (payload_len == 126) {
        uint8_t ext[2];
        if (recv_exact(fd, ext, 2) < 0) return "";
        payload_len = (static_cast<size_t>(ext[0]) << 8) | ext[1];
    }
    std::string payload(payload_len, '\0');
    if (payload_len > 0 && recv_exact(fd, payload.data(), payload_len) < 0) return "";
    return payload;
}

// ══════════════════════════════════════════════════════════════════════════════
// S1 — disconnect() before connect: 크래시 없음
// ══════════════════════════════════════════════════════════════════════════════

TEST(GuacWebClientTest, S1_DisconnectBeforeConnect) {
    GuacWebClient client([](const GuacInstruction&) {});
    ASSERT_NO_THROW(client.disconnect());
    EXPECT_FALSE(client.is_connected());
}

// ══════════════════════════════════════════════════════════════════════════════
// S2 — send_mouse() / send_key() before connect: 크래시 없음
// ══════════════════════════════════════════════════════════════════════════════

TEST(GuacWebClientTest, S2_InputBeforeConnect) {
    GuacWebClient client([](const GuacInstruction&) {});
    ASSERT_NO_THROW(client.send_mouse(100, 200, 1));
    ASSERT_NO_THROW(client.send_key(0x61, true));
    ASSERT_NO_THROW(client.send_key(0x61, false));
}

// ══════════════════════════════════════════════════════════════════════════════
// S3 — connect() → MockCdpServer → is_connected() == true
// ══════════════════════════════════════════════════════════════════════════════

TEST(GuacWebClientTest, S3_ConnectToMockServer) {
    MockCdpServer server;
    InstrCollector collector;

    GuacWebClient client(collector.callback());
    client.connect("https://example.com", 1280, 800, server.port());

    // size 명령어가 오면 connected_ == true로 전환됨
    ASSERT_TRUE(collector.wait_for_opcode("size"));
    EXPECT_TRUE(client.is_connected());

    client.disconnect();
}

// ══════════════════════════════════════════════════════════════════════════════
// S4 — connect() → disconnect() → is_connected() == false
// ══════════════════════════════════════════════════════════════════════════════

TEST(GuacWebClientTest, S4_DisconnectAfterConnect) {
    MockCdpServer server;
    InstrCollector collector;

    GuacWebClient client(collector.callback());
    client.connect("https://example.com", 1280, 800, server.port());
    ASSERT_TRUE(collector.wait_for_opcode("size"));
    EXPECT_TRUE(client.is_connected());

    client.disconnect();
    EXPECT_FALSE(client.is_connected());
}

// ══════════════════════════════════════════════════════════════════════════════
// S5 — size 명령어: width / height 아규먼트 확인
// ══════════════════════════════════════════════════════════════════════════════

TEST(GuacWebClientTest, S5_SizeInstructionArgs) {
    MockCdpServer server;
    InstrCollector collector;

    GuacWebClient client(collector.callback());
    client.connect("https://example.com", 640, 480, server.port());
    ASSERT_TRUE(collector.wait_for_opcode("size"));

    GuacInstruction sz = collector.first("size");
    ASSERT_EQ(sz.args.size(), 2u);
    EXPECT_EQ(sz.args[0], "640");
    EXPECT_EQ(sz.args[1], "480");

    client.disconnect();
}

// ══════════════════════════════════════════════════════════════════════════════
// S6 — captureScreenshot → img / blob / end 시퀀스 수신
// ══════════════════════════════════════════════════════════════════════════════

TEST(GuacWebClientTest, S6_ScreenshotInstructionSequence) {
    MockCdpServer server;
    InstrCollector collector;

    std::string jpeg = make_solid_b64_jpeg(8, 8, 255, 0, 0);  // 빨간 8×8
    ASSERT_FALSE(jpeg.empty());

    auto f = server.push_screenshot(jpeg);

    GuacWebClient client(collector.callback());
    client.connect("https://example.com", 1280, 800, server.port());
    ASSERT_TRUE(collector.wait_for_opcode("size"));

    // future가 resolve되면 응답 전송 완료
    ASSERT_EQ(f.wait_for(std::chrono::seconds(5)), std::future_status::ready);

    // img/blob/end가 모두 도착할 때까지 대기
    ASSERT_TRUE(collector.wait_for_opcode("end"));
    EXPECT_GE(collector.count("img"),  1);
    EXPECT_GE(collector.count("blob"), 1);
    EXPECT_GE(collector.count("end"),  1);

    client.disconnect();
}

// ══════════════════════════════════════════════════════════════════════════════
// S7 — delta: 동일 프레임 2회 → 두 번째는 blob/end 없음
// ══════════════════════════════════════════════════════════════════════════════

TEST(GuacWebClientTest, S7_DeltaSkipIdenticalFrame) {
    MockCdpServer server;
    InstrCollector collector;

    std::string jpeg = make_solid_b64_jpeg(8, 8, 0, 255, 0);  // 동일한 초록 8×8

    auto f1 = server.push_screenshot(jpeg);
    auto f2 = server.push_screenshot(jpeg);  // 동일 프레임

    GuacWebClient client(collector.callback());
    client.connect("https://example.com", 1280, 800, server.port());
    ASSERT_TRUE(collector.wait_for_opcode("size"));

    // 두 응답이 모두 전송될 때까지 대기
    ASSERT_EQ(f1.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    ASSERT_EQ(f2.wait_for(std::chrono::seconds(5)), std::future_status::ready);

    // flush_delta가 두 번째 응답을 처리할 시간을 준다
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 첫 프레임은 end 발생, 두 번째(동일)는 스킵 → end는 정확히 1개
    EXPECT_EQ(collector.count("end"), 1);

    client.disconnect();
}

// ══════════════════════════════════════════════════════════════════════════════
// S8 — delta: 다른 프레임 2회 → 두 번 모두 end 수신
// ══════════════════════════════════════════════════════════════════════════════

TEST(GuacWebClientTest, S8_DeltaSendDifferentFrame) {
    MockCdpServer server;
    InstrCollector collector;

    std::string jpeg_a = make_solid_b64_jpeg(8, 8, 255, 0, 0);  // 빨강
    std::string jpeg_b = make_solid_b64_jpeg(8, 8, 0, 0, 255);  // 파랑

    auto f1 = server.push_screenshot(jpeg_a);
    auto f2 = server.push_screenshot(jpeg_b);

    GuacWebClient client(collector.callback());
    client.connect("https://example.com", 1280, 800, server.port());
    ASSERT_TRUE(collector.wait_for_opcode("size"));

    ASSERT_EQ(f1.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    ASSERT_EQ(f2.wait_for(std::chrono::seconds(5)), std::future_status::ready);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 두 프레임 모두 달라서 각각 end 발생 → end 2개
    EXPECT_EQ(collector.count("end"), 2);

    client.disconnect();
}

// ══════════════════════════════════════════════════════════════════════════════
// S9 — send_mouse() → MockCdpServer에서 Input.dispatchMouseEvent 수신
// ══════════════════════════════════════════════════════════════════════════════

TEST(GuacWebClientTest, S9_SendMouseDispatchedToCdp) {
    MockCdpServer server;
    InstrCollector collector;

    GuacWebClient client(collector.callback());
    client.connect("https://example.com", 1280, 800, server.port());
    ASSERT_TRUE(collector.wait_for_opcode("size"));

    // 좌클릭 press
    client.send_mouse(100, 200, 1);

    ASSERT_TRUE(server.wait_for_method("Input.dispatchMouseEvent"));

    std::string params = server.last_params("Input.dispatchMouseEvent");
    EXPECT_NE(params.find("mousePressed"), std::string::npos);

    client.disconnect();
}

// ══════════════════════════════════════════════════════════════════════════════
// S10 — send_key() → MockCdpServer에서 Input.dispatchKeyEvent 수신
// ══════════════════════════════════════════════════════════════════════════════

TEST(GuacWebClientTest, S10_SendKeyDispatchedToCdp) {
    MockCdpServer server;
    InstrCollector collector;

    GuacWebClient client(collector.callback());
    client.connect("https://example.com", 1280, 800, server.port());
    ASSERT_TRUE(collector.wait_for_opcode("size"));

    // 'A' 키 (X11 keysym 0x41 = ASCII 'A')
    client.send_key(0x41, true);

    ASSERT_TRUE(server.wait_for_method("Input.dispatchKeyEvent"));

    std::string params = server.last_params("Input.dispatchKeyEvent");
    EXPECT_NE(params.find("keyDown"), std::string::npos);

    client.disconnect();
}

// ══════════════════════════════════════════════════════════════════════════════
// S11 — GuacWebSocketGateway start()/stop() 생명주기
// ══════════════════════════════════════════════════════════════════════════════

TEST(GuacWebSocketGatewayTest, S11_StartStopLifecycle) {
    uint16_t port = get_free_port();
    GuacWebSocketGateway gw;

    EXPECT_FALSE(gw.is_running());
    ASSERT_NO_THROW(gw.start(port));
    EXPECT_TRUE(gw.is_running());
    ASSERT_NO_THROW(gw.stop());
    EXPECT_FALSE(gw.is_running());
}

// ══════════════════════════════════════════════════════════════════════════════
// S12 — connect,web,...; 전송 → 크래시 없이 처리
//
// Chrome 미설치 환경에서는 GuacWebClient::connect()가 내부적으로 실패해
// is_connected()가 false로 유지된다. 게이트웨이는 크래시 없이 입력 루프를 유지하고
// Close 프레임을 받으면 정상 종료한다.
// ══════════════════════════════════════════════════════════════════════════════

TEST(GuacWebSocketGatewayTest, S12_WebProtocolRouting) {
    uint16_t gw_port = get_free_port();
    GuacWebSocketGateway gw;
    gw.start(gw_port);

    int fd = tcp_connect(gw_port);
    ASSERT_GE(fd, 0);
    ASSERT_TRUE(ws_handshake(fd, gw_port));

    // connect,web,https://example.com,1280,800;
    GuacInstruction connect_instr;
    connect_instr.opcode = "connect";
    connect_instr.args   = {"web", "https://example.com", "1280", "800"};
    ws_send_guac(fd, GuacParser::serialize(connect_instr));

    // 명령어 전송 후 게이트웨이가 처리할 시간을 준다
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Close 프레임 전송 → 게이트웨이 입력 루프 종료
    uint8_t close_frame[2] = {0x88, 0x00};  // FIN + Close opcode, len=0
    ::write(fd, close_frame, 2);

    close(fd);
    gw.stop();
    // 여기까지 크래시 없이 도달하면 통과
    SUCCEED();
}

// ══════════════════════════════════════════════════════════════════════════════
// S13 — 미지원 프로토콜 → error 명령어 응답 수신
// ══════════════════════════════════════════════════════════════════════════════

TEST(GuacWebSocketGatewayTest, S13_UnknownProtocolReturnsError) {
    uint16_t gw_port = get_free_port();
    GuacWebSocketGateway gw;
    gw.start(gw_port);

    int fd = tcp_connect(gw_port);
    ASSERT_GE(fd, 0);
    ASSERT_TRUE(ws_handshake(fd, gw_port));

    // 미지원 프로토콜
    GuacInstruction connect_instr;
    connect_instr.opcode = "connect";
    connect_instr.args   = {"unknown_protocol"};
    ws_send_guac(fd, GuacParser::serialize(connect_instr));

    // error 명령어가 포함된 WebSocket 프레임 수신 (타임아웃 2초)
    struct timeval tv{2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    std::string resp = ws_recv_frame(fd);

    EXPECT_NE(resp.find("error"), std::string::npos);

    close(fd);
    gw.stop();
}
