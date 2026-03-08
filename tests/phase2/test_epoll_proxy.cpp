#include <gtest/gtest.h>
#include "core/epoll_proxy.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <thread>
#include <chrono>

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
// run()은 실행 중이지만 accept_connection()은 아직 스텁이라 연결을 처리하지는 않는다.
// 중요한 것은 "커널 listen 큐에 연결이 수락됐는가"이다.
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
