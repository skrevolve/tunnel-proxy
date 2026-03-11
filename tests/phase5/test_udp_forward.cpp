#include <gtest/gtest.h>
#include "core/udp_proxy.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <thread>
#include <chrono>
#include <atomic>
#include <cstring>

// ── 공통 헬퍼 ─────────────────────────────────────────────────────────────────

// 사용 가능한 UDP 포트를 동적으로 할당한다.
// bind(0) → getsockname으로 커널이 배정한 포트 확인 → 소켓 닫기.
static int get_free_udp_port() {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = 0;
    bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    socklen_t len = sizeof(addr);
    getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    int port = ntohs(addr.sin_port);
    close(fd);
    return port;
}

// UDP 에코 서버
//
// recvfrom으로 수신한 패킷을 그 주소로 그대로 돌려보낸다.
// stop_flag가 true가 되면 루프를 빠져나간다.
// select 타임아웃 50ms로 stop_flag를 주기적으로 확인한다.
static void run_udp_echo_server(int server_fd, std::atomic<bool>& stop_flag) {
    char buf[65536];
    while (!stop_flag) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(server_fd, &rfds);
        timeval tv{ 0, 50000 };
        if (select(server_fd + 1, &rfds, nullptr, nullptr, &tv) <= 0) continue;

        sockaddr_in client_addr{};
        socklen_t   addr_len = sizeof(client_addr);
        ssize_t n = recvfrom(server_fd, buf, sizeof(buf), 0,
                             reinterpret_cast<sockaddr*>(&client_addr), &addr_len);
        if (n <= 0) continue;

        sendto(server_fd, buf, static_cast<size_t>(n), 0,
               reinterpret_cast<sockaddr*>(&client_addr), addr_len);
    }
}

// UDP 에코 서버 소켓 생성 + bind
static int make_udp_echo_server(int port) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(static_cast<uint16_t>(port));
    bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    return fd;
}

// 프록시에 UDP 패킷을 보내고 에코 응답을 받아 msg를 반환한다.
// 타임아웃(초) 내에 응답이 없으면 빈 문자열 반환.
static std::string udp_roundtrip(int proxy_port, const std::string& msg,
                                 int timeout_sec = 3) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);

    sockaddr_in proxy_addr{};
    proxy_addr.sin_family = AF_INET;
    proxy_addr.sin_port   = htons(static_cast<uint16_t>(proxy_port));
    inet_pton(AF_INET, "127.0.0.1", &proxy_addr.sin_addr);

    sendto(fd, msg.c_str(), msg.size(), 0,
           reinterpret_cast<sockaddr*>(&proxy_addr), sizeof(proxy_addr));

    // select로 응답 대기
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    timeval tv{ timeout_sec, 0 };
    if (select(fd + 1, &rfds, nullptr, nullptr, &tv) <= 0) {
        close(fd);
        return "";
    }

    char buf[65536]{};
    ssize_t n = recvfrom(fd, buf, sizeof(buf) - 1, 0, nullptr, nullptr);
    close(fd);
    if (n <= 0) return "";
    return std::string(buf, static_cast<size_t>(n));
}

// ── 테스트: 기본 양방향 포워딩 ────────────────────────────────────────────────
//
// 클라이언트 → 프록시(proxy_port) → 에코 서버(echo_port) → 프록시 → 클라이언트
// 에코 서버가 돌려준 데이터가 원본과 동일한지 확인.
TEST(UdpForward, BasicEcho) {
    int echo_port  = get_free_udp_port();
    int proxy_port = get_free_udp_port();

    int echo_fd = make_udp_echo_server(echo_port);
    std::atomic<bool> echo_stop{false};
    std::thread echo_t([&]() { run_udp_echo_server(echo_fd, echo_stop); });

    // session_timeout=5초, 짧게 설정해 테스트 속도 확보
    UdpProxy proxy(proxy_port, "127.0.0.1", echo_port, 64, 5);
    std::thread proxy_t([&proxy]() { proxy.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::string reply = udp_roundtrip(proxy_port, "hello");

    proxy.stop();
    proxy_t.join();
    echo_stop = true;
    echo_t.join();
    close(echo_fd);

    EXPECT_EQ(reply, "hello") << "에코 응답이 원본과 다름";
}

// ── 테스트: 다중 클라이언트 세션 분리 ────────────────────────────────────────
//
// 서로 다른 소켓(= 다른 포트)에서 각자 다른 메시지를 보낸다.
// 프록시가 세션을 분리해 각 클라이언트에게 자신의 메시지만 돌려줘야 한다.
TEST(UdpForward, MultiClientSessionIsolation) {
    int echo_port  = get_free_udp_port();
    int proxy_port = get_free_udp_port();

    int echo_fd = make_udp_echo_server(echo_port);
    std::atomic<bool> echo_stop{false};
    std::thread echo_t([&]() { run_udp_echo_server(echo_fd, echo_stop); });

    UdpProxy proxy(proxy_port, "127.0.0.1", echo_port, 64, 5);
    std::thread proxy_t([&proxy]() { proxy.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // 클라이언트 A, B 동시 송신
    std::string reply_a, reply_b;
    std::thread client_a([&]() {
        reply_a = udp_roundtrip(proxy_port, "client-A");
    });
    std::thread client_b([&]() {
        reply_b = udp_roundtrip(proxy_port, "client-B");
    });
    client_a.join();
    client_b.join();

    proxy.stop();
    proxy_t.join();
    echo_stop = true;
    echo_t.join();
    close(echo_fd);

    EXPECT_EQ(reply_a, "client-A") << "A의 응답이 잘못됨";
    EXPECT_EQ(reply_b, "client-B") << "B의 응답이 잘못됨";
}

// ── 테스트: 동일 클라이언트 반복 전송 ────────────────────────────────────────
//
// 같은 소켓으로 패킷을 여러 번 보낸다.
// 매번 새 세션을 만들지 않고 기존 세션(target_fd)을 재사용해야 한다.
TEST(UdpForward, SessionReuse) {
    int echo_port  = get_free_udp_port();
    int proxy_port = get_free_udp_port();

    int echo_fd = make_udp_echo_server(echo_port);
    std::atomic<bool> echo_stop{false};
    std::thread echo_t([&]() { run_udp_echo_server(echo_fd, echo_stop); });

    UdpProxy proxy(proxy_port, "127.0.0.1", echo_port, 64, 5);
    std::thread proxy_t([&proxy]() { proxy.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // 같은 소켓으로 3번 반복 전송
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in proxy_addr{};
    proxy_addr.sin_family = AF_INET;
    proxy_addr.sin_port   = htons(static_cast<uint16_t>(proxy_port));
    inet_pton(AF_INET, "127.0.0.1", &proxy_addr.sin_addr);

    bool all_ok = true;
    for (int i = 0; i < 3; i++) {
        std::string msg = "round-" + std::to_string(i);
        sendto(fd, msg.c_str(), msg.size(), 0,
               reinterpret_cast<sockaddr*>(&proxy_addr), sizeof(proxy_addr));

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        timeval tv{ 3, 0 };
        if (select(fd + 1, &rfds, nullptr, nullptr, &tv) <= 0) { all_ok = false; break; }

        char buf[256]{};
        ssize_t n = recvfrom(fd, buf, sizeof(buf) - 1, 0, nullptr, nullptr);
        if (std::string(buf, static_cast<size_t>(n)) != msg) { all_ok = false; break; }
    }
    close(fd);

    proxy.stop();
    proxy_t.join();
    echo_stop = true;
    echo_t.join();
    close(echo_fd);

    EXPECT_TRUE(all_ok) << "반복 전송 중 응답 불일치 또는 타임아웃";
}

// ── 테스트: 세션 타임아웃 자동 정리 ──────────────────────────────────────────
//
// session_timeout=1초로 설정 후 패킷 송수신.
// 1.5초 대기 뒤 cleanup이 실행되어 세션이 제거되었는지 확인.
// 이후 같은 소켓으로 다시 패킷을 보내면 새 세션이 생성되어 정상 응답이 와야 한다.
//
// 타임아웃 후 fd 재사용이 올바르게 동작하는지(= 좀비 세션이 남지 않는지) 검증.
TEST(UdpForward, SessionTimeout) {
    int echo_port  = get_free_udp_port();
    int proxy_port = get_free_udp_port();

    int echo_fd = make_udp_echo_server(echo_port);
    std::atomic<bool> echo_stop{false};
    std::thread echo_t([&]() { run_udp_echo_server(echo_fd, echo_stop); });

    // session_timeout=1초, cleanup_interval=1초로 설정.
    // → 1초 비활성 후 최대 1초 내에 cleanup이 실행되어 세션 정리.
    // 대기 시간 3초면 충분히 정리됨.
    UdpProxy proxy(proxy_port, "127.0.0.1", echo_port, 64, 1, 1);
    std::thread proxy_t([&proxy]() { proxy.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // 1차 전송 — 세션 생성
    std::string r1 = udp_roundtrip(proxy_port, "ping", 3);
    EXPECT_EQ(r1, "ping") << "1차 에코 실패";

    // 3초 대기: session_timeout(1초) + cleanup_interval(1초) 초과
    // → cleanup_expired_sessions()가 호출되어 세션 정리됨
    std::this_thread::sleep_for(std::chrono::seconds(3));

    // 2차 전송 — 기존 세션이 정리됐으므로 새 세션이 생성되어야 한다
    std::string r2 = udp_roundtrip(proxy_port, "pong", 3);

    proxy.stop();
    proxy_t.join();
    echo_stop = true;
    echo_t.join();
    close(echo_fd);

    EXPECT_EQ(r2, "pong") << "타임아웃 후 재연결 실패";
}
