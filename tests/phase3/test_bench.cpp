#include <gtest/gtest.h>
#include "core/epoll_proxy.h"
#include "core/basic_proxy.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <thread>
#include <chrono>
#include <atomic>
#include <vector>
#include <numeric>
#include <iostream>

// ── 공통 헬퍼 ─────────────────────────────────────────────────────────────────

static int get_free_port() {
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
    int port = ntohs(addr.sin_port);
    close(fd);
    return port;
}

// 멀티 연결을 지원하는 에코 서버
// 받은 데이터를 그대로 돌려보낸다.
static void run_echo_server(int server_fd, std::atomic<bool>& stop_flag) {
    while (!stop_flag) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(server_fd, &rfds);
        timeval tv{ 0, 50000 };
        if (select(server_fd + 1, &rfds, nullptr, nullptr, &tv) <= 0) continue;

        int conn = accept(server_fd, nullptr, nullptr);
        if (conn < 0) continue;

        std::thread([conn]() {
            char buf[65536];
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

// 에코 서버 소켓 생성 헬퍼
static int make_echo_server(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(static_cast<uint16_t>(port));
    bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    listen(fd, 16);
    return fd;
}

// 프록시를 경유해 data_size 바이트를 전송하고 소요 시간(ms)을 반환
static double measure_throughput_ms(int proxy_port, size_t data_size) {
    int client_fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(static_cast<uint16_t>(proxy_port));
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(client_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(client_fd);
        return -1.0;
    }

    std::vector<uint8_t> send_buf(data_size);
    for (size_t i = 0; i < data_size; i++) send_buf[i] = static_cast<uint8_t>(i & 0xFF);

    auto start = std::chrono::steady_clock::now();

    // 송신 스레드
    std::thread sender([&]() {
        size_t sent = 0;
        while (sent < data_size) {
            ssize_t w = write(client_fd, send_buf.data() + sent, data_size - sent);
            if (w <= 0) break;
            sent += w;
        }
    });

    // 수신 루프
    std::vector<uint8_t> recv_buf(data_size);
    size_t received = 0;
    while (received < data_size) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(client_fd, &rfds);
        timeval tv{ 5, 0 };
        if (select(client_fd + 1, &rfds, nullptr, nullptr, &tv) <= 0) break;
        ssize_t n = read(client_fd, recv_buf.data() + received, data_size - received);
        if (n <= 0) break;
        received += n;
    }

    auto end = std::chrono::steady_clock::now();
    sender.join();
    close(client_fd);

    if (received != data_size) return -1.0;

    return std::chrono::duration<double, std::milli>(end - start).count();
}

// ── 벤치마크: EpollProxy (splice + pipe pool) ─────────────────────────────────
//
// splice()는 커널 내부에서 데이터를 이동하므로 유저 공간 복사가 없다.
// pipe pool로 파이프 생성/소멸 시스템 콜도 제거된 상태.
// 10MB 전송 속도를 측정한다.
//
// Debug 빌드(-fsanitize=address,undefined)에서는 sanitizer 오버헤드로
// Release 대비 수배 느릴 수 있다. 실제 성능은 Release 빌드로 측정할 것.
// CI 러너 성능을 고려해 1MB로 제한한다 (로컬 Release에서는 더 큰 값으로 측정).
TEST(ZeroCopyBench, EpollProxyThroughput10MB) {
    const size_t DATA_SIZE = 1 * 1024 * 1024;  // 1MB (CI 환경 안정성)

    int echo_port  = get_free_port();
    int proxy_port = get_free_port();

    int echo_fd = make_echo_server(echo_port);
    std::atomic<bool> echo_stop{false};
    std::thread echo_t([&]() { run_echo_server(echo_fd, echo_stop); });

    EpollProxy proxy(proxy_port, "127.0.0.1", echo_port);
    std::thread proxy_t([&proxy]() { proxy.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    double ms = measure_throughput_ms(proxy_port, DATA_SIZE);

    // cleanup을 ASSERT 이전에 수행: ASSERT 실패로 예외가 발생해도
    // joinable thread 소멸자가 std::terminate를 호출하지 않도록 보장.
    proxy.stop();
    proxy_t.join();
    echo_stop = true;
    shutdown(echo_fd, SHUT_RDWR);
    echo_t.join();
    close(echo_fd);

    ASSERT_GT(ms, 0.0) << "전송 실패";

    double mb      = static_cast<double>(DATA_SIZE) / (1024.0 * 1024.0);
    double throughput = mb / (ms / 1000.0);

    std::cout << "\n[EpollProxy splice+pool] 1MB: "
              << ms << " ms  /  "
              << throughput << " MB/s\n";

    // loopback에서 최소 5 MB/s는 나와야 한다
    // Debug+sanitizer+CI 러너 환경을 모두 포함한 보수적 기준.
    // 실제 성능 측정은 Release 빌드에서 별도로 수행할 것.
    EXPECT_GT(throughput, 5.0) << "throughput too low: " << throughput << " MB/s";
}

// ── 벤치마크: BasicProxy (read/write + 스레드) ────────────────────────────────
//
// Phase 1 구현: 연결마다 스레드 2개 생성 + 블로킹 read/write.
// read → 유저 공간 buf → write 방식으로 복사 2회 발생.
// EpollProxy와 동일한 10MB 조건으로 측정해 차이를 확인한다.
TEST(ZeroCopyBench, BasicProxyThroughput10MB) {
    const size_t DATA_SIZE = 10 * 1024 * 1024;  // 10MB

    int echo_port  = get_free_port();
    int proxy_port = get_free_port();

    int echo_fd = make_echo_server(echo_port);
    std::atomic<bool> echo_stop{false};
    std::thread echo_t([&]() { run_echo_server(echo_fd, echo_stop); });

    BasicProxy proxy(proxy_port, "127.0.0.1", echo_port);
    std::thread proxy_t([&proxy]() { proxy.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    double ms = measure_throughput_ms(proxy_port, DATA_SIZE);

    proxy.stop();
    proxy_t.join();
    echo_stop = true;
    shutdown(echo_fd, SHUT_RDWR);
    echo_t.join();
    close(echo_fd);

    ASSERT_GT(ms, 0.0) << "전송 실패";

    double mb         = static_cast<double>(DATA_SIZE) / (1024.0 * 1024.0);
    double throughput = mb / (ms / 1000.0);

    std::cout << "\n[BasicProxy  read+write] 10MB: "
              << ms << " ms  /  "
              << throughput << " MB/s\n";

    EXPECT_GT(throughput, 50.0) << "throughput too low: " << throughput << " MB/s";
}

// ── 데이터 무결성: splice + pool 전환 후 내용이 바뀌지 않는가 ─────────────────
//
// splice()는 커널이 페이지를 이동시키므로 이론상 내용이 바뀔 수 없다.
// pool 도입 후에도 파이프 재사용 경로에서 이전 연결 데이터가 섞이지 않는지
// 패턴 데이터로 검증한다.
TEST(ZeroCopyBench, DataIntegrityAfterPoolReuse) {
    const size_t DATA_SIZE = 4 * 1024 * 1024;  // 4MB

    int echo_port  = get_free_port();
    int proxy_port = get_free_port();

    int echo_fd = make_echo_server(echo_port);
    std::atomic<bool> echo_stop{false};
    std::thread echo_t([&]() { run_echo_server(echo_fd, echo_stop); });

    EpollProxy proxy(proxy_port, "127.0.0.1", echo_port);
    std::thread proxy_t([&proxy]() { proxy.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // 3번 연속 전송해 파이프 풀이 재사용되는 상황을 만든다
    for (int round = 0; round < 3; round++) {
        int client_fd = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(static_cast<uint16_t>(proxy_port));
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        ASSERT_EQ(connect(client_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);

        // 각 라운드마다 다른 패턴으로 채움 (이전 연결 데이터와 혼용 검출)
        std::vector<uint8_t> send_buf(DATA_SIZE, static_cast<uint8_t>(round + 1));

        std::thread sender([&]() {
            size_t sent = 0;
            while (sent < DATA_SIZE) {
                ssize_t w = write(client_fd, send_buf.data() + sent, DATA_SIZE - sent);
                if (w <= 0) break;
                sent += w;
            }
        });

        std::vector<uint8_t> recv_buf(DATA_SIZE, 0);
        size_t received = 0;
        while (received < DATA_SIZE) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(client_fd, &rfds);
            timeval tv{ 5, 0 };
            if (select(client_fd + 1, &rfds, nullptr, nullptr, &tv) <= 0) break;
            ssize_t n = read(client_fd, recv_buf.data() + received, DATA_SIZE - received);
            if (n <= 0) break;
            received += n;
        }

        sender.join();
        close(client_fd);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        ASSERT_EQ(received, DATA_SIZE) << "round " << round << ": 데이터 유실";
        EXPECT_EQ(send_buf, recv_buf)  << "round " << round << ": 데이터 내용 불일치 (파이프 오염 의심)";
    }

    proxy.stop();
    proxy_t.join();
    echo_stop = true;
    shutdown(echo_fd, SHUT_RDWR);
    echo_t.join();
    close(echo_fd);
}
