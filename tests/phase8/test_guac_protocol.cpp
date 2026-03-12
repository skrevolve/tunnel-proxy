#include "core/guac_parser.h"
#include "core/guac_vnc.h"
#include "core/guac_ssh.h"
#include "core/guac_rdp.h"
#include "core/guac_websocket.h"

#include <gtest/gtest.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <csignal>
#include <openssl/evp.h>

#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <vector>
#include <string>
#include <cstring>
#include <pwd.h>

using namespace proxy;

// ── 공통 헬퍼: 사용 가능한 TCP 포트 ──────────────────────────────────────────

static uint16_t get_free_port() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
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

// ── InstrCollector: 콜백으로 수신한 GuacInstruction을 수집하고 대기한다 ──────

struct InstrCollector {
    std::mutex              mtx;
    std::condition_variable cv;
    std::vector<GuacInstruction> received;

    void push(const GuacInstruction& instr) {
        { std::lock_guard<std::mutex> lock(mtx); received.push_back(instr); }
        cv.notify_all();
    }

    GuacVncClient::InstructionCallback make_callback() {
        return [this](const GuacInstruction& instr) { push(instr); };
    }

    bool wait_for_opcode(const std::string& opcode,
                         std::chrono::seconds timeout = std::chrono::seconds(5)) {
        std::unique_lock<std::mutex> lock(mtx);
        return cv.wait_for(lock, timeout, [&] {
            for (const auto& i : received)
                if (i.opcode == opcode) return true;
            return false;
        });
    }
};

// ── MockVncServer: 최소 RFB 핸드셰이크 + FramebufferUpdate 1회 전송 ───────────
//
// libvncclient가 실제로 연결할 수 있는 RFB 3.8 서버를 인메모리로 구현한다.
//
// 프로토콜 흐름:
//   서버 → "RFB 003.008\n"         (버전 협상)
//   클라이언트 → "RFB 003.xxx\n"
//   서버 → [1, 1]                  (보안 타입 1개: None=1)
//   클라이언트 → [1]               (None 선택)
//   서버 → [0,0,0,0]               (SecurityResult OK)
//   클라이언트 → [shared_flag]     (ClientInit)
//   서버 → ServerInit(width, height, pixel_format, name)
//   클라이언트 → SetPixelFormat    (선택적)
//   클라이언트 → SetEncodings
//   클라이언트 → FramebufferUpdateRequest
//   서버 → FramebufferUpdate(Raw encoding, 64×64 회색 픽셀)

class MockVncServer {
public:
    static constexpr int W = 64;
    static constexpr int H = 64;

    MockVncServer() {
        listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
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
        listen(listen_fd_, 1);
        thread_ = std::thread(&MockVncServer::run, this);
    }

    ~MockVncServer() {
        stop_ = true;
        if (listen_fd_ >= 0) { shutdown(listen_fd_, SHUT_RDWR); close(listen_fd_); }
        if (thread_.joinable()) thread_.join();
    }

    uint16_t port() const { return port_; }

    // 클라이언트가 ServerInit까지 완료될 때까지 대기
    bool wait_connected(std::chrono::seconds timeout = std::chrono::seconds(5)) {
        std::unique_lock<std::mutex> lock(mtx_);
        return cv_.wait_for(lock, timeout, [this] { return connected_; });
    }

private:
    int            listen_fd_{-1};
    uint16_t       port_{0};
    std::thread    thread_;
    std::atomic<bool> stop_{false};
    std::mutex     mtx_;
    std::condition_variable cv_;
    bool           connected_{false};

    static void write_be16(int fd, uint16_t v) {
        uint8_t b[2] = {(uint8_t)(v >> 8), (uint8_t)(v & 0xFF)};
        write(fd, b, 2);
    }
    static void write_be32(int fd, uint32_t v) {
        uint8_t b[4] = {(uint8_t)(v>>24),(uint8_t)(v>>16),
                        (uint8_t)(v>>8), (uint8_t)(v&0xFF)};
        write(fd, b, 4);
    }

    void run() {
        sockaddr_in caddr{};
        socklen_t   len = sizeof(caddr);
        int cfd = accept(listen_fd_, reinterpret_cast<sockaddr*>(&caddr), &len);
        if (cfd < 0) return;

        // 1. 버전 협상
        write(cfd, "RFB 003.008\n", 12);
        char ver[12];
        recv(cfd, ver, 12, MSG_WAITALL);

        // 2. 보안 타입: None(1)
        uint8_t sec[2] = {1, 1};
        write(cfd, sec, 2);
        uint8_t chosen;
        recv(cfd, &chosen, 1, 0);

        // 3. SecurityResult OK
        uint8_t ok[4] = {0, 0, 0, 0};
        write(cfd, ok, 4);

        // 4. ClientInit
        uint8_t shared;
        recv(cfd, &shared, 1, 0);

        // 5. ServerInit
        write_be16(cfd, W);
        write_be16(cfd, H);
        // PixelFormat: bitsPerPixel=32, depth=24, bigEndian=0, trueColour=1,
        //              redMax=255, greenMax=255, blueMax=255,
        //              redShift=16, greenShift=8, blueShift=0, padding x3
        uint8_t pf[16] = {32, 24, 0, 1,
                          0, 255, 0, 255, 0, 255,   // redMax, greenMax, blueMax (2B BE each)
                          16, 8, 0,                  // shifts
                          0, 0, 0};                  // padding
        write(cfd, pf, 16);
        const char* name = "mock";
        write_be32(cfd, 4);
        write(cfd, name, 4);

        // ServerInit 완료 신호
        { std::lock_guard<std::mutex> lock(mtx_); connected_ = true; }
        cv_.notify_all();

        // 6. 클라이언트 설정 메시지 수신 후 FramebufferUpdateRequest에 응답
        while (!stop_) {
            uint8_t type;
            fd_set rfds; FD_ZERO(&rfds); FD_SET(cfd, &rfds);
            timeval tv{1, 0};
            if (select(cfd+1, &rfds, nullptr, nullptr, &tv) <= 0) continue;
            if (recv(cfd, &type, 1, 0) != 1) break;

            switch (type) {
            case 0: { // SetPixelFormat: 3 padding + 16 format = 19 bytes
                uint8_t buf[19];
                recv(cfd, buf, 19, MSG_WAITALL);
                break;
            }
            case 2: { // SetEncodings: 1 padding + 2 count + count*4 encoding IDs
                uint8_t hdr[3];
                recv(cfd, hdr, 3, MSG_WAITALL);
                uint16_t count = (static_cast<uint16_t>(hdr[1]) << 8) | hdr[2];
                std::vector<uint8_t> encs(count * 4);
                if (!encs.empty()) recv(cfd, encs.data(), encs.size(), MSG_WAITALL);
                break;
            }
            case 3: { // FramebufferUpdateRequest: 9 more bytes
                uint8_t req[9];
                recv(cfd, req, 9, MSG_WAITALL);
                send_framebuffer_update(cfd, 0, 0, W, H);
                break;
            }
            default:
                goto done;
            }
        }
        done:
        close(cfd);
    }

    void send_framebuffer_update(int fd, int x, int y, int w, int h) {
        // FramebufferUpdate: type=0, padding=0, num_rects=1 (2 bytes BE)
        uint8_t hdr[4] = {0, 0, 0, 1};
        write(fd, hdr, 4);

        // 사각형 헤더: x(2BE), y(2BE), w(2BE), h(2BE), encoding(4BE)=0(Raw)
        uint8_t rect[12];
        auto be16 = [](uint8_t* p, int v) {
            p[0] = (v >> 8) & 0xFF; p[1] = v & 0xFF;
        };
        be16(rect,     x); be16(rect+2, y);
        be16(rect+4,   w); be16(rect+6, h);
        rect[8] = rect[9] = rect[10] = rect[11] = 0; // Raw encoding
        write(fd, rect, 12);

        // 픽셀 데이터: BGRX 32bpp (회색)
        std::vector<uint8_t> pixels(w * h * 4, 0x80);
        write(fd, pixels.data(), pixels.size());
    }
};

// ── WebSocket 테스트 클라이언트 헬퍼 ─────────────────────────────────────────

static int tcp_connect_to(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    addr.sin_port = htons(port);
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd); return -1;
    }
    return fd;
}

// WebSocket HTTP 업그레이드 요청 전송 후 "101" 응답 확인
static bool ws_handshake(int fd, uint16_t port) {
    std::string req =
        "GET / HTTP/1.1\r\n"
        "Host: localhost:" + std::to_string(port) + "\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n";
    write(fd, req.data(), req.size());

    char buf[1024];
    ssize_t n = recv(fd, buf, sizeof(buf)-1, 0);
    if (n <= 0) return false;
    buf[n] = '\0';
    return strstr(buf, "101") != nullptr;
}

// 마스킹된 WebSocket 텍스트 프레임 전송 (클라이언트→서버)
static void ws_send_text(int fd, const std::string& text) {
    std::vector<uint8_t> frame;
    frame.push_back(0x81);  // FIN + Text
    size_t len = text.size();
    if (len < 126) {
        frame.push_back(0x80 | static_cast<uint8_t>(len));
    } else {
        frame.push_back(0x80 | 126);
        frame.push_back((len >> 8) & 0xFF);
        frame.push_back(len & 0xFF);
    }
    uint8_t mask[4] = {1, 2, 3, 4};
    frame.insert(frame.end(), mask, mask + 4);
    for (size_t i = 0; i < len; i++)
        frame.push_back(static_cast<uint8_t>(text[i]) ^ mask[i % 4]);
    write(fd, frame.data(), frame.size());
}

// 마스킹 없는 WebSocket 텍스트 프레임 수신 (서버→클라이언트)
static std::string ws_recv_text(int fd, int timeout_sec = 3) {
    timeval tv{timeout_sec, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t hdr[2];
    if (recv(fd, hdr, 2, MSG_WAITALL) != 2) return "";

    size_t len = hdr[1] & 0x7F;
    if (len == 126) {
        uint8_t ext[2];
        recv(fd, ext, 2, MSG_WAITALL);
        len = (static_cast<size_t>(ext[0]) << 8) | ext[1];
    }
    std::string payload(len, '\0');
    if (len > 0) recv(fd, payload.data(), len, MSG_WAITALL);
    return payload;
}

// ═════════════════════════════════════════════════════════════════════════════
// GuacVncClient — mock RFB 서버로 실제 연결 테스트
// ═════════════════════════════════════════════════════════════════════════════

// ── 시나리오 13: mock VNC 서버에 연결 → size instruction 수신 ─────────────────
//
// ServerInit 수신 후 GuacVncClient는 "size" instruction을 콜백으로 전달해야 한다.
// args[0]="0", args[1]=너비 문자열, args[2]=높이 문자열.
TEST(GuacVncClientProtocol, ConnectMockServer_ReceivesSizeInstruction) {
    signal(SIGPIPE, SIG_IGN);
    MockVncServer server;
    InstrCollector collector;

    GuacVncClient client(collector.make_callback());
    client.connect("127.0.0.1", server.port());

    ASSERT_TRUE(collector.wait_for_opcode("size", std::chrono::seconds(5)));

    bool found = false;
    {
        std::lock_guard<std::mutex> lock(collector.mtx);
        for (const auto& i : collector.received) {
            if (i.opcode == "size") {
                ASSERT_EQ(i.args.size(), 3u);
                EXPECT_EQ(i.args[1], std::to_string(MockVncServer::W));
                EXPECT_EQ(i.args[2], std::to_string(MockVncServer::H));
                found = true;
                break;
            }
        }
    }
    EXPECT_TRUE(found);
    client.disconnect();
}

// ── 시나리오 14: mock 서버가 FramebufferUpdate 전송 → img/blob/end 수신 ────────
//
// mock 서버가 Raw encoding FramebufferUpdate를 보내면
// GuacVncClient는 img → blob... → end GuacInstruction 시퀀스를 전달해야 한다.
TEST(GuacVncClientProtocol, FramebufferUpdate_ReceivesImgBlobEndSequence) {
    signal(SIGPIPE, SIG_IGN);
    MockVncServer server;
    InstrCollector collector;

    GuacVncClient client(collector.make_callback());
    client.connect("127.0.0.1", server.port());

    // mock 서버가 FramebufferUpdateRequest를 받고 Update를 보낼 때까지 대기
    ASSERT_TRUE(collector.wait_for_opcode("img", std::chrono::seconds(5)));
    ASSERT_TRUE(collector.wait_for_opcode("blob", std::chrono::seconds(5)));
    ASSERT_TRUE(collector.wait_for_opcode("end", std::chrono::seconds(5)));

    client.disconnect();
}

// ── 시나리오 15: disconnect() 후 is_connected() == false ─────────────────────
//
// disconnect() 호출 후 반드시 is_connected()가 false를 반환해야 한다.
TEST(GuacVncClientProtocol, Disconnect_IsConnectedFalse) {
    signal(SIGPIPE, SIG_IGN);
    MockVncServer server;
    InstrCollector collector;

    GuacVncClient client(collector.make_callback());
    client.connect("127.0.0.1", server.port());

    // 연결이 완료될 때까지 대기 (size instruction 기다림)
    collector.wait_for_opcode("size", std::chrono::seconds(5));

    client.disconnect();
    EXPECT_FALSE(client.is_connected());
}

// ═════════════════════════════════════════════════════════════════════════════
// GuacSshClient — loopback sshd로 실제 연결 테스트
// (CI에서 openssh-server + runner 계정 비밀번호 설정 필요)
// ═════════════════════════════════════════════════════════════════════════════

// ── 시나리오 16: SSH 연결 → size + pipe instruction 수신 ─────────────────────
//
// SSH 셸 세션이 수립되면 GuacSshClient는 size → pipe 순서로 instruction을 전달한다.
TEST(GuacSshClientProtocol, ConnectLocalSshd_ReceivesSizeAndPipe) {
    // sshd 가용성 확인
    int check = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    addr.sin_port = htons(22);
    if (connect(check, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(check);
        GTEST_SKIP() << "sshd not available on port 22";
    }
    close(check);

    const char* user = getenv("USER");
    if (!user) { struct passwd* pw = getpwuid(getuid()); user = pw ? pw->pw_name : "runner"; }

    InstrCollector collector;
    GuacSshClient client(collector.make_callback());
    client.connect("127.0.0.1", 22, user, "testpass");

    EXPECT_TRUE(collector.wait_for_opcode("pipe", std::chrono::seconds(10)));

    // pipe 수신 전에 size도 수신되었는지 확인
    bool has_size = false;
    {
        std::lock_guard<std::mutex> lock(collector.mtx);
        for (const auto& i : collector.received)
            if (i.opcode == "size") { has_size = true; break; }
    }
    EXPECT_TRUE(has_size);

    client.disconnect();
}

// ── 시나리오 17: send_input() 후 blob instruction 수신 ───────────────────────
//
// 키보드 입력(echo 명령)을 보내면 터미널 출력이 blob instruction으로 반환되어야 한다.
TEST(GuacSshClientProtocol, SendInput_ReceivesBlobInstruction) {
    int check = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    addr.sin_port = htons(22);
    if (connect(check, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(check);
        GTEST_SKIP() << "sshd not available on port 22";
    }
    close(check);

    const char* user = getenv("USER");
    if (!user) { struct passwd* pw = getpwuid(getuid()); user = pw ? pw->pw_name : "runner"; }

    InstrCollector collector;
    GuacSshClient client(collector.make_callback());
    client.connect("127.0.0.1", 22, user, "testpass");

    // 셸이 준비될 때까지 대기
    ASSERT_TRUE(collector.wait_for_opcode("pipe", std::chrono::seconds(10)));

    // 명령 입력
    client.send_input("echo guac_ssh_test\n");
    EXPECT_TRUE(collector.wait_for_opcode("blob", std::chrono::seconds(5)));

    client.disconnect();
}

// ── 시나리오 18: disconnect() → end instruction 수신, is_connected() false ────
TEST(GuacSshClientProtocol, Disconnect_ReceivesEndAndIsConnectedFalse) {
    int check = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    addr.sin_port = htons(22);
    if (connect(check, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(check);
        GTEST_SKIP() << "sshd not available on port 22";
    }
    close(check);

    const char* user = getenv("USER");
    if (!user) { struct passwd* pw = getpwuid(getuid()); user = pw ? pw->pw_name : "runner"; }

    InstrCollector collector;
    GuacSshClient client(collector.make_callback());
    client.connect("127.0.0.1", 22, user, "testpass");
    ASSERT_TRUE(collector.wait_for_opcode("pipe", std::chrono::seconds(10)));

    client.disconnect();

    EXPECT_TRUE(collector.wait_for_opcode("end", std::chrono::seconds(3)));
    EXPECT_FALSE(client.is_connected());
}

// ═════════════════════════════════════════════════════════════════════════════
// GuacRdpClient — 연결 실패 graceful 처리
// ═════════════════════════════════════════════════════════════════════════════

// ── 시나리오 19: 닫힌 포트로 RDP 연결 → is_connected() false (크래시 없음) ─────
//
// FreeRDP이 연결 실패를 처리하고 GuacRdpClient가 gracefully 종료해야 한다.
// connected_ 플래그가 false를 유지해야 한다.
TEST(GuacRdpClientProtocol, ConnectionRefused_IsConnectedFalse) {
    InstrCollector collector;
    GuacRdpClient client(collector.make_callback());

    // 리슨 중인 서버가 없는 포트에 연결 시도
    EXPECT_NO_THROW(client.connect("127.0.0.1", 13389, "user", "pass"));

    std::this_thread::sleep_for(std::chrono::seconds(3));
    EXPECT_FALSE(client.is_connected());
    client.disconnect();  // 이미 연결 안 됐어도 안전해야 함
}

// ═════════════════════════════════════════════════════════════════════════════
// GuacWebSocketGateway — 라이프사이클
// ═════════════════════════════════════════════════════════════════════════════

// ── 시나리오 20: start() 후 is_running() == true ──────────────────────────────
TEST(GuacWebSocketLifecycle, StartSetsRunning) {
    uint16_t port = get_free_port();
    GuacWebSocketGateway gw;
    EXPECT_FALSE(gw.is_running());
    gw.start(port);
    EXPECT_TRUE(gw.is_running());
    gw.stop();
}

// ── 시나리오 21: stop() 후 is_running() == false ─────────────────────────────
TEST(GuacWebSocketLifecycle, StopClearsRunning) {
    uint16_t port = get_free_port();
    GuacWebSocketGateway gw;
    gw.start(port);
    gw.stop();
    EXPECT_FALSE(gw.is_running());
}

// ── 시나리오 22: stop() 후 재 start() 정상 동작 ──────────────────────────────
TEST(GuacWebSocketLifecycle, RestartAfterStop_Works) {
    uint16_t port = get_free_port();
    GuacWebSocketGateway gw;
    gw.start(port);
    gw.stop();
    EXPECT_NO_THROW(gw.start(port));
    EXPECT_TRUE(gw.is_running());
    gw.stop();
}

// ═════════════════════════════════════════════════════════════════════════════
// GuacWebSocketGateway — WebSocket 프로토콜 통신
// ═════════════════════════════════════════════════════════════════════════════

// ── 시나리오 23: WebSocket HTTP upgrade → 101 Switching Protocols ────────────
//
// 브라우저가 WebSocket 업그레이드 요청을 보내면 게이트웨이는
// "101 Switching Protocols" 응답을 보내야 한다.
TEST(GuacWebSocketProtocol, HttpUpgrade_Receives101Response) {
    uint16_t port = get_free_port();
    GuacWebSocketGateway gw;
    gw.start(port);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    int fd = tcp_connect_to(port);
    ASSERT_GE(fd, 0);
    EXPECT_TRUE(ws_handshake(fd, port));

    close(fd);
    gw.stop();
}

// ── 시나리오 24: 업그레이드 후 Guacamole 텍스트 프레임 송신 ─────────────────
//
// 핸드셰이크 완료 후 Guacamole 명령어 텍스트 프레임을 보낼 수 있어야 한다.
// 게이트웨이가 프레임을 수신하고 크래시 없이 처리해야 한다.
TEST(GuacWebSocketProtocol, SendGuacFrame_GatewayProcessesWithoutCrash) {
    uint16_t port = get_free_port();
    GuacWebSocketGateway gw;
    gw.start(port);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    int fd = tcp_connect_to(port);
    ASSERT_GE(fd, 0);
    ASSERT_TRUE(ws_handshake(fd, port));

    // "size" 명령어 전송 (connect 아니어서 게이트웨이가 error를 보내거나 연결을 닫을 수 있음)
    ws_send_text(fd, "4.size,1.0,4.1024,3.768;");
    // 크래시 없이 처리되면 성공
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    close(fd);
    gw.stop();
}

// ── 시나리오 25: connect instruction + 미지원 프로토콜 → error instruction ─────
//
// 알 수 없는 프로토콜 타입으로 connect 명령어를 보내면
// 게이트웨이는 "error" opcode의 GuacInstruction을 텍스트 프레임으로 반환해야 한다.
TEST(GuacWebSocketProtocol, ConnectUnknownProtocol_ReturnsErrorInstruction) {
    uint16_t port = get_free_port();
    GuacWebSocketGateway gw;
    gw.start(port);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    int fd = tcp_connect_to(port);
    ASSERT_GE(fd, 0);
    ASSERT_TRUE(ws_handshake(fd, port));

    // "foo" 프로토콜은 지원하지 않음
    // connect: args=["foo","127.0.0.1","1234"]
    ws_send_text(fd, "7.connect,3.foo,9.127.0.0.1,4.1234;");

    std::string response = ws_recv_text(fd, 3);
    ASSERT_FALSE(response.empty()) << "Gateway should respond with error instruction";

    // 응답을 GuacInstruction으로 파싱
    GuacParser parser;
    parser.feed(response);
    ASSERT_TRUE(parser.has_instruction());
    auto instr = parser.next_instruction();
    EXPECT_EQ(instr.opcode, "error");

    close(fd);
    gw.stop();
}
