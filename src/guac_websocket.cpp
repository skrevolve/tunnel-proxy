/**
 * @file guac_websocket.cpp
 * @brief Phase 8-E — WebSocket 서버 + Guacamole 백엔드 라우팅
 *
 * ── WebSocket 핸드셰이크 ──────────────────────────────────────────────────
 *
 *   RFC 6455 §4.2.2:
 *   1. HTTP 요청에서 Sec-WebSocket-Key 추출
 *   2. key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11" 연결
 *   3. SHA-1 해시 → base64 인코딩 → Sec-WebSocket-Accept
 *   4. "HTTP/1.1 101 Switching Protocols" 응답 전송
 *
 * ── WebSocket 프레임 ─────────────────────────────────────────────────────
 *
 *   클라이언트→서버 프레임은 항상 마스킹됨 (RFC 6455 §5.3).
 *   서버→클라이언트 프레임은 마스킹 없음.
 *   Payload 길이 인코딩: 7비트(<126), 2바이트(126), 8바이트(127).
 *
 * ── 백엔드 라우팅 ────────────────────────────────────────────────────────
 *
 *   첫 번째 WebSocket 프레임 = Guacamole "connect" 명령어.
 *   args[0] == "rdp"/"ssh"/"vnc"에 따라 해당 클라이언트를 생성한다.
 *   미지원 프로토콜은 "error" 명령어를 보내고 연결을 닫는다.
 */

#include "core/guac_websocket.h"
#include "core/guac_parser.h"

#include <openssl/evp.h>   // EVP_DigestInit/Update/Final (SHA-1), EVP_EncodeBlock

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <stdexcept>
#include <cstring>
#include <vector>
#include <mutex>
#include <memory>

namespace proxy {

// ── GuacWebSocketGateway::Impl ────────────────────────────────────────────

struct GuacWebSocketGateway::Impl {
    std::atomic<bool> stop{false};
};

// ── 연결당 상태 ───────────────────────────────────────────────────────────

struct WsSession {
    int               fd;
    std::atomic<bool> active{true};
    std::mutex        send_mutex;

    std::unique_ptr<GuacRdpClient>  rdp;
    std::unique_ptr<GuacSshClient>  ssh;
    std::unique_ptr<GuacVncClient>  vnc;
    std::unique_ptr<GuacWebClient>  web;
    std::unique_ptr<GuacHttpClient> http;

    explicit WsSession(int f) : fd(f) {}
};

// ── 저수준 I/O 헬퍼 ──────────────────────────────────────────────────────

static ssize_t recv_exact(int fd, void* buf, size_t n) {
    size_t total = 0;
    while (total < n) {
        ssize_t r = recv(fd, static_cast<char*>(buf) + total, n - total, MSG_WAITALL);
        if (r <= 0) return r == 0 ? static_cast<ssize_t>(total) : -1;
        total += r;
    }
    return static_cast<ssize_t>(total);
}

// HTTP 요청을 \r\n\r\n이 나올 때까지 읽는다.
static std::string read_http_request(int fd) {
    std::string req;
    req.reserve(1024);
    char byte;
    while (req.size() < 8192) {
        if (recv(fd, &byte, 1, 0) <= 0) return "";
        req += byte;
        if (req.size() >= 4 &&
            req.compare(req.size() - 4, 4, "\r\n\r\n") == 0) break;
    }
    return req;
}

// ── WebSocket 핸드셰이크 ──────────────────────────────────────────────────

static bool do_ws_handshake(int fd) {
    std::string req = read_http_request(fd);
    if (req.empty()) return false;

    const std::string KEY_HEADER = "Sec-WebSocket-Key:";
    size_t pos = req.find(KEY_HEADER);
    if (pos == std::string::npos) return false;
    pos += KEY_HEADER.size();
    while (pos < req.size() && req[pos] == ' ') pos++;
    size_t end = req.find("\r\n", pos);
    if (end == std::string::npos) return false;
    std::string key = req.substr(pos, end - pos);

    // Sec-WebSocket-Accept = base64(SHA1(key + GUID))
    const std::string GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string combined = key + GUID;
    unsigned char sha1[20];
    unsigned int  sha1_len = 20;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha1(), nullptr);
    EVP_DigestUpdate(ctx, combined.data(), combined.size());
    EVP_DigestFinal_ex(ctx, sha1, &sha1_len);
    EVP_MD_CTX_free(ctx);

    char b64[64];
    int  b64_len = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(b64), sha1, 20);
    std::string accept(b64, static_cast<size_t>(b64_len));

    std::string response =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
    return ::write(fd, response.data(), response.size()) > 0;
}

// ── WebSocket 프레임 읽기 (클라이언트→서버, 마스킹됨) ────────────────────

static std::string read_ws_frame(int fd) {
    uint8_t hdr[2];
    if (recv_exact(fd, hdr, 2) != 2) return "";

    uint8_t opcode = hdr[0] & 0x0F;
    if (opcode == 8) return ""; // Close 프레임

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

    if (masked) {
        for (size_t i = 0; i < payload_len; i++) payload[i] ^= mask[i % 4];
    }
    return payload;
}

// ── WebSocket 프레임 전송 (서버→클라이언트, 마스킹 없음) ──────────────────

static void send_ws_frame(int fd, const std::string& text) {
    std::vector<uint8_t> frame;
    frame.push_back(0x81);  // FIN + Text opcode

    size_t len = text.size();
    if (len < 126) {
        frame.push_back(static_cast<uint8_t>(len));
    } else if (len < 65536) {
        frame.push_back(126);
        frame.push_back((len >> 8) & 0xFF);
        frame.push_back( len       & 0xFF);
    } else {
        frame.push_back(127);
        for (int i = 7; i >= 0; i--)
            frame.push_back((len >> (i * 8)) & 0xFF);
    }
    frame.insert(frame.end(), text.begin(), text.end());
    ::write(fd, frame.data(), frame.size());
}

// ── GuacWebSocketGateway 구현 ─────────────────────────────────────────────

GuacWebSocketGateway::GuacWebSocketGateway()
    : impl_(std::make_unique<Impl>())
{}

void GuacWebSocketGateway::set_web_renderer(const std::string& renderer) {
    web_renderer_ = renderer;
}

void GuacWebSocketGateway::set_tunnel_server(TunnelServer* ts) {
    tunnel_server_ = ts;
}

// URL에서 host와 port를 추출한다.
// "http://host:8080/path" → ("host", 8080)
// "https://host/path"    → ("host", 443)
static std::pair<std::string, uint16_t> parse_url_host_port(const std::string& url) {
    bool is_https = url.size() >= 5 && url.substr(0, 5) == "https";
    uint16_t default_port = is_https ? 443 : 80;

    size_t scheme_end = url.find("://");
    if (scheme_end == std::string::npos) return {"", default_port};

    size_t host_start = scheme_end + 3;
    size_t path_start = url.find('/', host_start);
    std::string host_port = (path_start != std::string::npos)
        ? url.substr(host_start, path_start - host_start)
        : url.substr(host_start);

    size_t colon = host_port.find(':');
    if (colon != std::string::npos) {
        try {
            int port = std::stoi(host_port.substr(colon + 1));
            return {host_port.substr(0, colon), static_cast<uint16_t>(port)};
        } catch (...) {}
    }
    return {host_port, default_port};
}

GuacWebSocketGateway::~GuacWebSocketGateway() {
    stop();
}

void GuacWebSocketGateway::start(uint16_t port) {
    if (running_.load()) return;

    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0)
        throw std::runtime_error("socket: " + std::string(strerror(errno)));

    int opt = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);

    if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
        throw std::runtime_error("bind: " + std::string(strerror(errno)));
    if (listen(listen_fd_, SOMAXCONN) < 0)
        throw std::runtime_error("listen: " + std::string(strerror(errno)));

    impl_->stop = false;
    running_    = true;
    accept_thread_ = std::thread(&GuacWebSocketGateway::accept_loop, this);
}

void GuacWebSocketGateway::stop() {
    if (!running_.load()) return;
    impl_->stop = true;
    running_    = false;
    if (listen_fd_ >= 0) {
        shutdown(listen_fd_, SHUT_RDWR);
        close(listen_fd_);
        listen_fd_ = -1;
    }
    if (accept_thread_.joinable()) accept_thread_.join();
}

bool GuacWebSocketGateway::is_running() const {
    return running_.load();
}

void GuacWebSocketGateway::accept_loop() {
    while (!impl_->stop.load()) {
        sockaddr_in client_addr{};
        socklen_t   len = sizeof(client_addr);
        int client_fd = accept(listen_fd_,
                               reinterpret_cast<sockaddr*>(&client_addr), &len);
        if (client_fd < 0) {
            if (errno == EINTR || errno == EBADF || errno == EINVAL) break;
            continue;
        }
        std::thread(&GuacWebSocketGateway::handle_connection, this, client_fd).detach();
    }
}

void GuacWebSocketGateway::handle_connection(int fd) {
    if (!do_ws_handshake(fd)) { close(fd); return; }

    auto session = std::make_shared<WsSession>(fd);

    // 첫 번째 프레임 = connect 명령어
    std::string frame = read_ws_frame(fd);
    if (frame.empty()) { close(fd); return; }

    GuacParser parser;
    try { parser.feed(frame); } catch (...) { close(fd); return; }
    if (!parser.has_instruction()) { close(fd); return; }

    GuacInstruction instr = parser.next_instruction();
    if (instr.opcode != "connect" || instr.args.empty()) {
        GuacInstruction err;
        err.opcode = "error";
        err.args   = {"Expected connect instruction", "1001"};
        send_ws_frame(fd, GuacParser::serialize(err));
        close(fd);
        return;
    }

    const std::string protocol = instr.args[0];

    // 백엔드 → WebSocket 전달 콜백
    // weak_ptr 사용: cb가 session->ssh->callback_에 저장되면
    // session → ssh → callback_ → lambda → session 순환 참조가 생겨 메모리 누수 발생
    std::weak_ptr<WsSession> weak_session = session;
    auto cb = [weak_session](const GuacInstruction& gi) {
        auto s = weak_session.lock();
        if (!s || !s->active.load()) return;
        std::string text = GuacParser::serialize(gi);
        std::lock_guard<std::mutex> lock(s->send_mutex);
        send_ws_frame(s->fd, text);
    };

    auto get = [&](size_t idx, const std::string& def) {
        return instr.args.size() > idx ? instr.args[idx] : def;
    };

    if (protocol == "vnc") {
        uint16_t port = static_cast<uint16_t>(std::stoi(get(2, "5900")));
        session->vnc = std::make_unique<GuacVncClient>(cb);
        session->vnc->connect(get(1, "localhost"), port, get(3, ""));
    } else if (protocol == "ssh") {
        uint16_t port = static_cast<uint16_t>(std::stoi(get(2, "22")));
        session->ssh = std::make_unique<GuacSshClient>(cb);
        session->ssh->connect(get(1, "localhost"), port, get(3, ""), get(4, ""));
    } else if (protocol == "rdp") {
        uint16_t port = static_cast<uint16_t>(std::stoi(get(2, "3389")));
        session->rdp = std::make_unique<GuacRdpClient>(cb);
        session->rdp->connect(get(1, "localhost"), port, get(3, ""), get(4, ""));
    } else if (protocol == "web") {
        // 형식 A (직접):  connect,web,<url>;
        // 형식 B (터널):  connect,web,<url>,<agent_id>;
        // 형식 C (CDP):   connect,web,<url>,<width>,<height>;  (web_renderer=chromium)
        const std::string url      = get(1, "about:blank");
        const std::string arg2     = get(2, "");

        // web_renderer=chromium 이면서 arg2가 숫자(너비)인 경우 → Chromium 스트리밍
        bool is_chromium = (web_renderer_ == "chromium");

        if (is_chromium) {
            int w = arg2.empty() ? 1280 : std::stoi(arg2);
            int h = std::stoi(get(3, "800"));
            session->web = std::make_unique<GuacWebClient>(cb);
            session->web->connect(url, w, h);
        } else {
            // web_renderer=http — 직접 또는 터널 경유
            const std::string agent_id = arg2;  // 비어있으면 직접 연결

            session->http = std::make_unique<GuacHttpClient>(cb);

            if (!agent_id.empty() && tunnel_server_) {
                // 터널 경유 HTTP: connect_via_tunnel 호출 → pre_fd로 curl
                // connect_via_tunnel은 OPEN_ACK 대기로 블로킹 → 워커 스레드에서 실행
                auto session_ref = session;  // shared_ptr 복사 — 세션 생존 보장
                std::thread([this, session_ref, url, agent_id]() {
                    auto [host, port] = parse_url_host_port(url);
                    if (host.empty()) {
                        GuacInstruction err;
                        err.opcode = "error";
                        err.args   = {"Invalid URL for tunnel routing: " + url, "400"};
                        std::lock_guard<std::mutex> lock(session_ref->send_mutex);
                        send_ws_frame(session_ref->fd, GuacParser::serialize(err));
                        return;
                    }

                    int pre_fd = tunnel_server_->connect_via_tunnel(agent_id, host, port);
                    if (pre_fd < 0) {
                        GuacInstruction err;
                        err.opcode = "error";
                        err.args   = {"Tunnel connect failed: agent=" + agent_id, "502"};
                        std::lock_guard<std::mutex> lock(session_ref->send_mutex);
                        send_ws_frame(session_ref->fd, GuacParser::serialize(err));
                        return;
                    }

                    if (session_ref->http && session_ref->active.load()) {
                        session_ref->http->connect(pre_fd, url);
                    } else {
                        close(pre_fd);
                    }
                }).detach();
            } else {
                // 직접 HTTP GET
                session->http->connect(url);
            }
        }
    } else {
        GuacInstruction err;
        err.opcode = "error";
        err.args   = {"Unknown protocol: " + protocol, "1001"};
        send_ws_frame(fd, GuacParser::serialize(err));
        close(fd);
        return;
    }

    // 브라우저 → 백엔드 전달 루프
    parser.reset();
    while (session->active.load()) {
        std::string f = read_ws_frame(fd);
        if (f.empty()) break;
        try {
            parser.feed(f);
        } catch (...) {
            // 파싱 불가 프레임(IME 특수문자 등)은 무시하고 계속 진행
            parser.reset();
            continue;
        }
        while (parser.has_instruction()) {
            GuacInstruction ci = parser.next_instruction();
            // SSH: "key" 명령어 → send_input
            if (session->ssh && ci.opcode == "key" && !ci.args.empty())
                session->ssh->send_input(ci.args[0]);
            // Web: "mouse" / "key" 명령어 → CDP Input 이벤트
            if (session->web) {
                if (ci.opcode == "mouse" && ci.args.size() >= 3) {
                    session->web->send_mouse(
                        std::stoi(ci.args[0]),  // x
                        std::stoi(ci.args[1]),  // y
                        std::stoi(ci.args[2])   // button_mask
                    );
                } else if (ci.opcode == "key" && ci.args.size() >= 2) {
                    session->web->send_key(
                        std::stoi(ci.args[0]),   // keysym
                        ci.args[1] == "1"        // pressed
                    );
                }
            }
            // TODO(Phase 9): RDP/VNC 마우스·키보드 입력 처리
        }
    }

    session->active = false;
    if (session->rdp)  session->rdp->disconnect();
    if (session->ssh)  session->ssh->disconnect();
    if (session->vnc)  session->vnc->disconnect();
    if (session->web)  session->web->disconnect();
    if (session->http) session->http->disconnect();
    close(fd);
}

} // namespace proxy
