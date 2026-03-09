#include <gtest/gtest.h>
#include "core/epoll_proxy.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <thread>
#include <chrono>
#include <atomic>
#include <string>

// ── 헬퍼: 사용 가능한 임시 포트를 할당받는다 ──────────────────────────────────
//
// 테스트마다 고정 포트를 쓰면 이전 테스트의 TIME_WAIT 상태와 충돌할 수 있다.
// port=0으로 bind하면 커널이 비어있는 포트를 골라 할당한다.
// 이 함수는 그 포트 번호를 반환하고 소켓을 바로 닫는다.
static int get_free_port() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    EXPECT_GE(fd, 0);

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = 0;  // 커널이 빈 포트 자동 선택

    EXPECT_EQ(bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);

    socklen_t len = sizeof(addr);
    EXPECT_EQ(getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len), 0);

    int port = ntohs(addr.sin_port);
    close(fd);
    return port;
}

// ── 테스트 1: 생성자 — 정상 초기화 ────────────────────────────────────────────
//
// EpollProxy를 만들면 예외 없이 생성되고
// epoll_fd와 listen_fd가 모두 열려있어야 한다.
// 초기 연결 카운터는 0이어야 한다.
TEST(EpollProxyInit, ConstructorSucceeds) {
    int port = get_free_port();
    EXPECT_NO_THROW({
        EpollProxy proxy(port, "127.0.0.1", 9999);
        EXPECT_EQ(proxy.get_total_connections(),  0u);
        EXPECT_EQ(proxy.get_active_connections(), 0u);
    });
}

// ── 테스트 2: 포트 충돌 — 같은 포트 이중 바인드 감지 ─────────────────────────
//
// 같은 포트에 두 번째 EpollProxy를 생성하면
// bind()가 "Address already in use"로 실패해 예외가 던져져야 한다.
// 이걸 감지 못하면 두 프록시가 같은 포트를 공유하는 혼란이 생긴다.
TEST(EpollProxyInit, PortConflictThrows) {
    int port = get_free_port();
    EpollProxy proxy1(port, "127.0.0.1", 9999);

    EXPECT_THROW({
        EpollProxy proxy2(port, "127.0.0.1", 9999);
    }, std::runtime_error);
}

// ── 테스트 3: stop() — 다중 호출 안전성 ───────────────────────────────────────
//
// stop()을 여러 번 호출해도 크래시나 예외가 없어야 한다.
// 소멸자도 stop()을 호출하므로 double-stop 시나리오는 실제로 발생한다.
TEST(EpollProxyLifecycle, StopMultipleTimesIsSafe) {
    int port = get_free_port();
    EpollProxy proxy(port, "127.0.0.1", 9999);

    EXPECT_NO_THROW({
        proxy.stop();
        proxy.stop();
        proxy.stop();
    });
}

// ── 테스트 4: run() + stop() — 이벤트 루프 시작/종료 ─────────────────────────
//
// run()을 별도 스레드에서 실행하고 stop()으로 종료할 수 있어야 한다.
// 타임아웃 내에 스레드가 종료되면 epoll_wait 루프가 정상 동작한 것.
TEST(EpollProxyLifecycle, RunAndStop) {
    int port = get_free_port();
    EpollProxy proxy(port, "127.0.0.1", 9999);

    std::thread t([&proxy]() {
        proxy.run();
    });

    // 루프가 올라올 시간을 준다
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    proxy.stop();

    // 최대 1초 안에 스레드가 끝나야 한다
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    bool joined = false;
    while (std::chrono::steady_clock::now() < deadline) {
        if (t.joinable()) {
            t.join();
            joined = true;
            break;
        }
    }
    if (t.joinable()) t.join();  // 타임아웃 시 강제 join

    EXPECT_TRUE(joined) << "run() did not return after stop()";
}

// ── 테스트 5: 포트 수신 확인 — 실제로 연결을 받을 수 있는가 ───────────────────
//
// EpollProxy가 바인딩된 포트에 TCP 연결을 시도해
// connect()가 성공(= 커널 수준에서 SYN-ACK 반환)하는지 검증한다.
TEST(EpollProxyNetwork, SocketIsListening) {
    int port = get_free_port();
    EpollProxy proxy(port, "127.0.0.1", 9999);

    std::thread t([&proxy]() { proxy.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    // 클라이언트 소켓으로 connect 시도
    int client_fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(client_fd, 0);

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(static_cast<uint16_t>(port));
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    int ret = connect(client_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    close(client_fd);

    proxy.stop();
    t.join();

    EXPECT_EQ(ret, 0) << "connect() failed — proxy is not listening on port " << port;
}

// ── 헬퍼: 에코 서버 ────────────────────────────────────────────────────────────
//
// 클라이언트가 보낸 데이터를 그대로 돌려보내는 단순 TCP 서버.
// Phase 2-C 포워딩 테스트에서 "타겟 서버" 역할을 한다.
// stop_flag가 세팅되면 accept 루프를 종료한다.
static void run_echo_server(int server_fd, std::atomic<bool>& stop_flag) {
    while (!stop_flag) {
        // accept 타임아웃을 위해 select 사용
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(server_fd, &rfds);
        timeval tv{ 0, 100000 };  // 100ms
        if (select(server_fd + 1, &rfds, nullptr, nullptr, &tv) <= 0) continue;

        int conn = accept(server_fd, nullptr, nullptr);
        if (conn < 0) continue;

        // 에코: 받은 데이터를 그대로 돌려보낸다
        std::thread([conn]() {
            char buf[4096];
            while (true) {
                ssize_t n = read(conn, buf, sizeof(buf));
                if (n <= 0) break;
                ssize_t w = 0;
                while (w < n) {
                    ssize_t r = write(conn, buf + w, n - w);
                    if (r <= 0) goto done;
                    w += r;
                }
            }
            done:
            close(conn);
        }).detach();
    }
}

// ── 테스트 6: 단방향 포워딩 — client → proxy → echo server → proxy → client ──
//
// 에코 서버를 타겟으로 두고 EpollProxy를 경유해 데이터를 전송한다.
// 클라이언트가 보낸 메시지가 에코 서버를 거쳐 그대로 돌아와야 한다.
// 이게 통과하면 accept_connection, forward_data, close_connection이
// 모두 올바르게 동작하는 것이다.
TEST(EpollProxyForward, EchoRoundTrip) {
    // 에코 서버 준비
    int echo_port  = get_free_port();
    int proxy_port = get_free_port();

    int echo_server_fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(echo_server_fd, 0);
    int opt = 1;
    setsockopt(echo_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in echo_addr{};
    echo_addr.sin_family      = AF_INET;
    echo_addr.sin_addr.s_addr = INADDR_ANY;
    echo_addr.sin_port        = htons(static_cast<uint16_t>(echo_port));
    ASSERT_EQ(bind(echo_server_fd, reinterpret_cast<sockaddr*>(&echo_addr), sizeof(echo_addr)), 0);
    ASSERT_EQ(listen(echo_server_fd, 16), 0);

    std::atomic<bool> echo_stop{false};
    std::thread echo_t([&]() { run_echo_server(echo_server_fd, echo_stop); });

    // 프록시 시작
    EpollProxy proxy(proxy_port, "127.0.0.1", echo_port);
    std::thread proxy_t([&proxy]() { proxy.run(); });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // 클라이언트 연결
    int client_fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(client_fd, 0);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(static_cast<uint16_t>(proxy_port));
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    ASSERT_EQ(connect(client_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    // 데이터 송신
    const std::string msg = "hello epoll";
    ssize_t sent = write(client_fd, msg.c_str(), msg.size());
    EXPECT_EQ(sent, static_cast<ssize_t>(msg.size()));

    // 에코 수신
    char buf[64] = {};
    // 최대 500ms 동안 응답을 기다린다
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(client_fd, &rfds);
    timeval tv{ 0, 500000 };
    int ready = select(client_fd + 1, &rfds, nullptr, nullptr, &tv);
    ASSERT_GT(ready, 0) << "timed out waiting for echo";

    ssize_t n = read(client_fd, buf, sizeof(buf) - 1);
    EXPECT_GT(n, 0);
    EXPECT_EQ(std::string(buf, n), msg);

    // 정리
    close(client_fd);
    proxy.stop();
    proxy_t.join();

    echo_stop = true;
    shutdown(echo_server_fd, SHUT_RDWR);
    echo_t.join();
    close(echo_server_fd);
}

// ── 테스트 7: 연결 카운터 — 연결/해제 시 카운터가 정확한가 ───────────────────
//
// 클라이언트가 연결되면 active_connections가 1 증가하고
// 연결을 닫으면 1 감소해야 한다.
// 카운터가 틀리면 운영 시 연결 수 모니터링이 불가능하다.
TEST(EpollProxyForward, ConnectionCountTracking) {
    int echo_port  = get_free_port();
    int proxy_port = get_free_port();

    int echo_server_fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(echo_server_fd, 0);
    int opt = 1;
    setsockopt(echo_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in echo_addr{};
    echo_addr.sin_family      = AF_INET;
    echo_addr.sin_addr.s_addr = INADDR_ANY;
    echo_addr.sin_port        = htons(static_cast<uint16_t>(echo_port));
    ASSERT_EQ(bind(echo_server_fd, reinterpret_cast<sockaddr*>(&echo_addr), sizeof(echo_addr)), 0);
    ASSERT_EQ(listen(echo_server_fd, 16), 0);

    std::atomic<bool> echo_stop{false};
    std::thread echo_t([&]() { run_echo_server(echo_server_fd, echo_stop); });

    EpollProxy proxy(proxy_port, "127.0.0.1", echo_port);
    std::thread proxy_t([&proxy]() { proxy.run(); });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    EXPECT_EQ(proxy.get_total_connections(),  0u);
    EXPECT_EQ(proxy.get_active_connections(), 0u);

    // 연결 수립
    int client_fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(client_fd, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(static_cast<uint16_t>(proxy_port));
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    ASSERT_EQ(connect(client_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(proxy.get_total_connections(),  1u);
    EXPECT_EQ(proxy.get_active_connections(), 1u);

    // 연결 해제 — 클라이언트가 FIN을 보내면 proxy가 EOF를 감지하고 close_connection 호출
    close(client_fd);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(proxy.get_active_connections(), 0u);
    EXPECT_EQ(proxy.get_total_connections(),  1u);  // total은 감소하지 않음

    proxy.stop();
    proxy_t.join();

    echo_stop = true;
    shutdown(echo_server_fd, SHUT_RDWR);
    echo_t.join();
    close(echo_server_fd);
}

// ── 테스트 8: 대용량 데이터 — splice partial 전송 누락 없는가 ─────────────────
//
// splice()는 파이프 버퍼 크기(64KB) 단위로 처리한다.
// 512KB를 전송해 partial splice 루프가 데이터를 유실 없이 전달하는지 검증한다.
// 바이트 수와 내용이 모두 일치해야 통과한다.
TEST(EpollProxyForward, LargeDataNoLoss) {
    const size_t DATA_SIZE = 512 * 1024;  // 512KB

    int echo_port  = get_free_port();
    int proxy_port = get_free_port();

    int echo_server_fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(echo_server_fd, 0);
    int opt = 1;
    setsockopt(echo_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in echo_addr{};
    echo_addr.sin_family      = AF_INET;
    echo_addr.sin_addr.s_addr = INADDR_ANY;
    echo_addr.sin_port        = htons(static_cast<uint16_t>(echo_port));
    ASSERT_EQ(bind(echo_server_fd, reinterpret_cast<sockaddr*>(&echo_addr), sizeof(echo_addr)), 0);
    ASSERT_EQ(listen(echo_server_fd, 16), 0);

    std::atomic<bool> echo_stop{false};
    std::thread echo_t([&]() { run_echo_server(echo_server_fd, echo_stop); });

    EpollProxy proxy(proxy_port, "127.0.0.1", echo_port);
    std::thread proxy_t([&proxy]() { proxy.run(); });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    int client_fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(client_fd, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(static_cast<uint16_t>(proxy_port));
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    ASSERT_EQ(connect(client_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);

    // 송신 데이터 생성 (반복 패턴)
    std::vector<uint8_t> send_buf(DATA_SIZE);
    for (size_t i = 0; i < DATA_SIZE; i++) send_buf[i] = static_cast<uint8_t>(i & 0xFF);

    // 별도 스레드에서 전송 (수신 루프와 동시에 진행)
    std::thread sender([&]() {
        size_t sent = 0;
        while (sent < DATA_SIZE) {
            ssize_t w = write(client_fd, send_buf.data() + sent, DATA_SIZE - sent);
            if (w <= 0) break;
            sent += w;
        }
    });

    // 수신 루프 — 전부 받을 때까지 대기
    std::vector<uint8_t> recv_buf(DATA_SIZE);
    size_t received = 0;
    while (received < DATA_SIZE) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(client_fd, &rfds);
        timeval tv{ 2, 0 };  // 2초 타임아웃
        if (select(client_fd + 1, &rfds, nullptr, nullptr, &tv) <= 0) break;
        ssize_t n = read(client_fd, recv_buf.data() + received, DATA_SIZE - received);
        if (n <= 0) break;
        received += n;
    }

    sender.join();

    EXPECT_EQ(received, DATA_SIZE) << "데이터 유실: " << (DATA_SIZE - received) << " bytes";
    EXPECT_EQ(send_buf, recv_buf) << "데이터 내용 불일치";

    close(client_fd);
    proxy.stop();
    proxy_t.join();

    echo_stop = true;
    shutdown(echo_server_fd, SHUT_RDWR);
    echo_t.join();
    close(echo_server_fd);
}
