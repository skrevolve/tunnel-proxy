#include "core/basic_proxy.h"
#include "utils/logger.h"
#include <sys/socket.h>   // socket, setsockopt, bind, listen, accept, connect, shutdown
#include <netinet/in.h>   // sockaddr_in, INADDR_ANY, htons
#include <arpa/inet.h>    // inet_pton
#include <unistd.h>       // close, read, write
#include <cstring>        // strerror
#include <thread>         // std::thread
#include <stdexcept>      // std::runtime_error
#include <cerrno>         // errno

// ── 생성자 / 소멸자 ────────────────────────────────────────────────────────────

BasicProxy::BasicProxy(int local_port, const std::string& target_ip, int target_port)
    : listen_fd_(-1)        // -1 = 아직 소켓 없음
    , target_ip_(target_ip)
    , target_port_(target_port)
    , running_(false)
    , total_connections_(0)
    , active_connections_(0)
{
    // 생성자에서 소켓을 만들어 두면 run() 전에 포트 충돌을 바로 감지할 수 있다.
    // 실패하면 예외가 발생해 BasicProxy 객체 자체가 생성되지 않는다.
    listen_fd_ = create_listening_socket(local_port);
}

BasicProxy::~BasicProxy() {
    // RAII 패턴: 객체 수명이 끝나면 자원을 반드시 정리한다.
    // stop() 먼저 → accept() 블로킹 해제
    // 그 다음 close() → 소켓 fd 반환
    stop();
    if (listen_fd_ >= 0) {
        close(listen_fd_);
        listen_fd_ = -1;  // 중복 close 방지
    }
}

// ── 메인 루프 ──────────────────────────────────────────────────────────────────

void BasicProxy::run() {
    running_ = true;
    Logger::info("Proxy started, listening on fd: " + std::to_string(listen_fd_));

    while (running_) {
        // accept()는 클라이언트가 연결할 때까지 블로킹한다.
        // 두 번째, 세 번째 인자(addr, addrlen)는 클라이언트 주소 정보인데,
        // 지금은 필요 없으므로 nullptr로 무시한다.
        // (필요하면 sockaddr_storage와 socklen_t를 넘겨 IP/포트를 얻을 수 있다)
        int client_fd = accept(listen_fd_, nullptr, nullptr);

        if (client_fd < 0) {
            // stop()이 shutdown(listen_fd_)를 호출하면 accept()가 -1을 반환한다.
            // 이때 running_이 false면 정상 종료이므로 루프를 빠져나간다.
            if (!running_) break;

            // 그 외 에러(EMFILE: fd 한도 초과 등)는 로그만 남기고 계속 대기한다.
            // 에러 하나로 프록시 전체를 종료하지 않는다.
            Logger::error("accept failed: " + std::string(strerror(errno)));
            continue;
        }

        // detach()를 쓰는 이유:
        //   join()을 쓰면 run()이 스레드가 끝날 때까지 블로킹되어
        //   다음 accept()를 못 한다. 동시 연결 처리가 불가능해진다.
        //   detach()는 스레드를 백그라운드에서 독립적으로 실행시키고
        //   run()은 즉시 다음 accept()로 넘어간다.
        //
        //   단점: detach된 스레드의 수명을 추적하기 어렵다.
        //         프로그램 종료 시 진행 중인 스레드가 강제 종료될 수 있다.
        //         Phase 11(안정성)에서 스레드 풀로 개선 예정.
        std::thread([this, client_fd]() {
            handle_connection(client_fd);
        }).detach();
    }
}

void BasicProxy::stop() {
    running_ = false;

    // accept()는 블로킹 시스템 콜이라 running_을 false로만 바꿔도
    // 다음 연결이 올 때까지 accept()가 깨어나지 않는다.
    // shutdown(SHUT_RDWR)으로 listen_fd_의 읽기/쓰기를 강제로 닫으면
    // accept()가 즉시 -1(EINVAL)을 반환하고 루프를 탈출할 수 있다.
    if (listen_fd_ >= 0) {
        shutdown(listen_fd_, SHUT_RDWR);
    }
}

uint64_t BasicProxy::get_total_connections() const  { return total_connections_.load(); }
uint64_t BasicProxy::get_active_connections() const { return active_connections_.load(); }

// ── 소켓 생성 ──────────────────────────────────────────────────────────────────

int BasicProxy::create_listening_socket(int port) {
    Logger::debug("Creating listening socket on port " + std::to_string(port));

    // socket(도메인, 타입, 프로토콜)
    //   AF_INET    : IPv4
    //   SOCK_STREAM: TCP (연결 기반, 순서 보장, 신뢰성 있는 전송)
    //   0          : 프로토콜 자동 선택 (SOCK_STREAM이면 TCP)
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        throw std::runtime_error("socket: " + std::string(strerror(errno)));
    }

    // SO_REUSEADDR 설정:
    //   TCP 연결이 끊긴 후 TIME_WAIT 상태가 일정 시간(보통 60초) 유지된다.
    //   이 상태에서 같은 포트를 bind()하면 "Address already in use" 에러 발생.
    //   SO_REUSEADDR을 설정하면 TIME_WAIT 중에도 즉시 같은 포트를 bind() 가능.
    //   개발 중 프록시를 자주 재시작할 때 60초를 기다릴 필요가 없어진다.
    int opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        close(fd);  // 이후 단계에서 실패 시 반드시 fd를 닫아야 fd 누수가 없다.
        throw std::runtime_error("setsockopt(SO_REUSEADDR): " + std::string(strerror(errno)));
    }

    // sockaddr_in 구조체 설정
    // {}로 zero-initialization: 패딩 바이트까지 0으로 초기화해 쓰레기값 방지
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;      // IPv4
    addr.sin_addr.s_addr = INADDR_ANY;   // 0.0.0.0 — 모든 네트워크 인터페이스에서 수신
    addr.sin_port        = htons(static_cast<uint16_t>(port));
    // htons (host-to-network short):
    //   네트워크 프로토콜은 big-endian을 사용한다.
    //   x86 CPU는 little-endian이므로 바이트 순서를 변환해야 한다.
    //   예) port 8080 = 0x1F90 → little-endian: 90 1F → big-endian: 1F 90

    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        throw std::runtime_error("bind: " + std::string(strerror(errno)));
    }
    // bind() 이후 이 소켓은 port 번호와 연결되었다.
    // 아직 클라이언트 연결을 받을 준비는 안 된 상태.

    // SOMAXCONN: 커널이 허용하는 최대 대기 연결 수 (보통 128~4096)
    // accept() 호출 전에 연결 요청이 오면 이 큐에 쌓인다.
    // 큐가 가득 차면 새 연결 요청이 거부(RST)된다.
    if (listen(fd, SOMAXCONN) < 0) {
        close(fd);
        throw std::runtime_error("listen: " + std::string(strerror(errno)));
    }

    return fd;
}

// ── 타겟 연결 ──────────────────────────────────────────────────────────────────

int BasicProxy::connect_to_target(const std::string& ip, int port) {
    Logger::debug("Connecting to " + ip + ":" + std::to_string(port));

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        throw std::runtime_error("socket: " + std::string(strerror(errno)));
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(static_cast<uint16_t>(port));

    // inet_pton (presentation-to-network):
    //   문자열 IP("127.0.0.1")를 네트워크 바이트 순서의 이진 값으로 변환.
    //   반환값:
    //     1  → 성공
    //     0  → 유효하지 않은 IP 형식 (예: "999.999.999.999")
    //    -1  → 지원하지 않는 주소 체계 (AF_INET 이외)
    //   inet_addr()을 쓰지 않는 이유:
    //     실패 시 INADDR_NONE(0xFFFFFFFF)을 반환하는데 이 값이
    //     유효한 IP인 255.255.255.255와 동일해 에러를 구분할 수 없다.
    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) <= 0) {
        close(fd);
        throw std::runtime_error("inet_pton: invalid IP address: " + ip);
    }

    // connect()는 3-way handshake가 완료될 때까지 블로킹한다.
    // 타겟 서버가 없거나 포트가 닫혀있으면 ECONNREFUSED로 실패한다.
    // 타임아웃(기본 ~75초)이 있어 서버가 응답 없으면 오래 걸릴 수 있다.
    // Phase 2에서 논블로킹 connect로 개선 예정.
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        throw std::runtime_error("connect to " + ip + ":" + std::to_string(port) +
                                 " failed: " + strerror(errno));
    }

    return fd;
}

// ── 연결 처리 ──────────────────────────────────────────────────────────────────

void BasicProxy::handle_connection(int client_fd) {
    total_connections_++;
    active_connections_++;

    Logger::info("New connection (fd=" + std::to_string(client_fd) + ") total=" +
                 std::to_string(total_connections_.load()));

    // 타겟 서버 연결 실패 시 클라이언트 연결도 정리하고 종료
    int target_fd = -1;
    try {
        target_fd = connect_to_target(target_ip_, target_port_);
    } catch (const std::exception& e) {
        Logger::error("Failed to connect to target: " + std::string(e.what()));
        active_connections_--;
        close(client_fd);
        return;
    }

    // 양방향 포워딩을 위한 스레드 2개 생성
    //   t1: client → target (클라이언트가 보낸 요청을 타겟으로 전달)
    //   t2: target → client (타겟의 응답을 클라이언트로 전달)
    //
    // 람다 캡처 [this, client_fd, target_fd]:
    //   this        : BasicProxy 멤버(target_ip_, target_port_)에 접근하기 위해
    //   client_fd, target_fd : 값 복사. fd는 int이므로 복사가 안전하다.
    std::thread t1([this, client_fd, target_fd]() {
        forward_data(client_fd, target_fd);
    });
    std::thread t2([this, target_fd, client_fd]() {
        forward_data(target_fd, client_fd);
    });

    // join() — 두 스레드가 모두 끝날 때까지 대기
    // detach가 아닌 join을 쓰는 이유:
    //   close(client_fd)와 close(target_fd)를 스레드 종료 후에 해야
    //   forward_data가 실행 중에 fd가 닫히는 것을 방지할 수 있다.
    //   스레드가 끝나야 fd를 더 이상 사용하지 않는다는 것이 보장된다.
    t1.join();
    t2.join();

    close(client_fd);
    close(target_fd);

    active_connections_--;
    Logger::debug("Connection closed (fd=" + std::to_string(client_fd) + ")");
}

// ── 데이터 포워딩 ──────────────────────────────────────────────────────────────

void BasicProxy::forward_data(int from_fd, int to_fd) {
    // 4096 바이트 = 일반적인 페이지 크기(4KB)와 동일.
    // 너무 작으면 read/write 시스템 콜 횟수가 늘어 오버헤드 증가.
    // 너무 크면 스택 사용량이 늘어나고 L1 캐시를 넘어선다.
    // Phase 3(zero-copy)에서 버퍼 없이 splice()로 대체 예정.
    constexpr size_t BUF_SIZE = 4096;
    char buffer[BUF_SIZE];

    while (true) {
        // read()가 블로킹하다가 데이터가 오면 읽은 바이트 수를 반환한다.
        //   n > 0 : 정상 읽기
        //   n == 0: EOF (상대방이 연결을 정상 종료, FIN 수신)
        //   n < 0 : 에러 (EINTR: 시그널로 인한 중단, 재시도 가능 등)
        ssize_t n = read(from_fd, buffer, BUF_SIZE);
        if (n <= 0) break;  // EOF 또는 에러 → 포워딩 중단

        // partial write 처리:
        //   TCP 전송 버퍼가 가득 차면 write()가 요청한 바이트보다
        //   적게 전송하고 실제 전송된 바이트 수를 반환한다.
        //   나머지를 전송할 때까지 반복하지 않으면 데이터가 유실된다.
        ssize_t written = 0;
        while (written < n) {
            ssize_t w = write(to_fd, buffer + written, static_cast<size_t>(n - written));
            if (w <= 0) return;  // 쓰기 실패(상대방 연결 종료 등) → 즉시 반환
            written += w;
        }
    }

    // 한 방향이 끊기면 반대 방향의 쓰기 끝을 닫는다.
    //
    // shutdown(to_fd, SHUT_WR)을 쓰는 이유:
    //   close(to_fd)를 하면 반대 방향 스레드(t2)도 이 fd로 read()하고 있어
    //   갑자기 fd가 닫히면 예기치 않은 동작이 생길 수 있다.
    //   SHUT_WR은 쓰기만 닫아 to_fd로 FIN을 보내고,
    //   반대쪽 read()가 0(EOF)을 반환해 정상적으로 종료되게 한다.
    //   fd 자체는 닫지 않으므로 다른 스레드의 read()는 계속 유효하다.
    shutdown(to_fd, SHUT_WR);
}
