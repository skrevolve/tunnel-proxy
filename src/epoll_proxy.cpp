#include "core/epoll_proxy.h"
#include "utils/logger.h"
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>      // fcntl, O_NONBLOCK
#include <unistd.h>
#include <vector>
#include <cstring>
#include <stdexcept>
#include <cerrno>

// ── 생성자 / 소멸자 ────────────────────────────────────────────────────────────

EpollProxy::EpollProxy(int local_port, const std::string& target_ip, int target_port,
                       int max_events)
    : listen_fd_(-1)
    , epoll_fd_(-1)
    , target_ip_(target_ip)
    , target_port_(target_port)
    , max_events_(max_events)
    , running_(false)
    , total_connections_(0)
    , active_connections_(0)
{
    // 1. epoll 인스턴스 생성
    //    EPOLL_CLOEXEC: exec() 계열 호출 시 자식 프로세스에 fd 누출 방지
    epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0) {
        throw std::runtime_error("epoll_create1: " + std::string(strerror(errno)));
    }

    // 2. 논블로킹 리스닝 소켓 생성
    //    ET 모드에서 블로킹 소켓을 쓰면 accept()/read()가 무한 대기
    try {
        listen_fd_ = create_listening_socket(local_port);
    } catch (...) {
        close(epoll_fd_);
        epoll_fd_ = -1;
        throw;
    }

    // 3. listen_fd를 epoll에 등록해 새 연결 이벤트 감지
    //    EPOLLIN:  새 연결 도착 (= "읽을 수 있는 상태")
    //    EPOLLET:  edge-triggered 모드 (상태 변화 시 1번만 이벤트 발생)
    add_to_epoll(listen_fd_, EPOLLIN | EPOLLET);
}

EpollProxy::~EpollProxy() {
    stop();
    // epoll_fd를 먼저 닫으면 등록된 fd에 대한 이벤트 감시가 중단된다.
    // 그 다음 listen_fd를 닫는다.
    if (epoll_fd_  >= 0) { close(epoll_fd_);  epoll_fd_  = -1; }
    if (listen_fd_ >= 0) { close(listen_fd_); listen_fd_ = -1; }
}

// ── 메인 이벤트 루프 ───────────────────────────────────────────────────────────

void EpollProxy::run() {
    running_ = true;

    // 이벤트 배열: epoll_wait이 준비된 이벤트를 여기에 채워준다.
    // max_events_: 한 번의 epoll_wait 호출에서 반환할 최대 이벤트 수.
    // 너무 크면 메모리 낭비, 너무 작으면 이벤트가 밀려 지연 발생.
    std::vector<epoll_event> events(max_events_);

    Logger::info("[EpollProxy] event loop started");

    while (running_) {
        // epoll_wait(epoll_fd, 결과배열, 최대개수, 타임아웃ms)
        // 타임아웃 -1: I/O 이벤트가 생길 때까지 무한 대기
        // 반환값: 준비된 이벤트 수 (0이면 타임아웃, -1이면 에러)
        int n = epoll_wait(epoll_fd_, events.data(), max_events_, -1);

        if (n < 0) {
            // EINTR: 시그널(SIGINT 등)로 epoll_wait가 중단됨 → 재시도
            if (errno == EINTR) continue;
            Logger::error("[EpollProxy] epoll_wait: " + std::string(strerror(errno)));
            break;
        }

        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;

            if (fd == listen_fd_) {
                // 새 클라이언트 연결 요청 (Phase 2-C에서 구현)
                accept_connection();
            } else {
                // 기존 연결에서 데이터 수신 또는 에러 (Phase 2-C에서 구현)
                handle_event(fd, events[i].events);
            }
        }
    }

    Logger::info("[EpollProxy] event loop stopped");
    running_ = false;
}

void EpollProxy::stop() {
    running_ = false;
    // epoll_wait 블로킹을 해제하기 위해 listen_fd를 shutdown한다.
    // BasicProxy::stop()과 동일한 방식.
    if (listen_fd_ >= 0) shutdown(listen_fd_, SHUT_RDWR);
}

uint64_t EpollProxy::get_total_connections() const  { return total_connections_.load(); }
uint64_t EpollProxy::get_active_connections() const { return active_connections_.load(); }

// ── 소켓 초기화 (Phase 2-B에서 구현) ──────────────────────────────────────────

int EpollProxy::create_listening_socket(int port) {
    Logger::debug("[EpollProxy] creating listening socket on port " + std::to_string(port));

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        throw std::runtime_error("socket: " + std::string(strerror(errno)));
    }

    int opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        close(fd);
        throw std::runtime_error("setsockopt(SO_REUSEADDR): " + std::string(strerror(errno)));
    }

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(static_cast<uint16_t>(port));

    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        throw std::runtime_error("bind: " + std::string(strerror(errno)));
    }

    if (listen(fd, SOMAXCONN) < 0) {
        close(fd);
        throw std::runtime_error("listen: " + std::string(strerror(errno)));
    }

    // BasicProxy와의 핵심 차이: 논블로킹 설정
    // ET 모드에서 accept()가 블로킹이면 대기 연결이 없을 때 이벤트 루프 전체가 멈춘다.
    set_nonblocking(fd);

    return fd;
}

void EpollProxy::set_nonblocking(int fd) {
    // F_GETFL로 현재 플래그를 읽은 뒤 O_NONBLOCK을 OR해서 설정한다.
    // 기존 플래그(O_RDWR 등)를 보존하면서 논블로킹만 추가하기 위해 OR를 사용한다.
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        throw std::runtime_error("fcntl(F_GETFL): " + std::string(strerror(errno)));
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        throw std::runtime_error("fcntl(F_SETFL, O_NONBLOCK): " + std::string(strerror(errno)));
    }
}

// ── epoll 관리 (Phase 2-B에서 구현) ───────────────────────────────────────────

void EpollProxy::add_to_epoll(int fd, uint32_t events) {
    epoll_event ev{};
    ev.events  = events;
    ev.data.fd = fd;  // epoll_wait 결과에서 어떤 fd에 이벤트가 발생했는지 식별
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
        throw std::runtime_error("epoll_ctl(ADD): " + std::string(strerror(errno)));
    }
}

void EpollProxy::mod_epoll(int fd, uint32_t events) {
    epoll_event ev{};
    ev.events  = events;
    ev.data.fd = fd;
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) < 0) {
        throw std::runtime_error("epoll_ctl(MOD): " + std::string(strerror(errno)));
    }
}

void EpollProxy::remove_from_epoll(int fd) {
    // Linux 2.6.9 이후: 마지막 인자 nullptr 사용 가능
    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
}

// ── 타겟 연결 ─────────────────────────────────────────────────────────────────

int EpollProxy::connect_to_target(const std::string& ip, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        throw std::runtime_error("socket: " + std::string(strerror(errno)));
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) <= 0) {
        close(fd);
        throw std::runtime_error("inet_pton: invalid address " + ip);
    }

    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        throw std::runtime_error("connect to " + ip + ":" + std::to_string(port) +
                                 " failed: " + strerror(errno));
    }

    return fd;
}

// ── 이벤트 처리 ───────────────────────────────────────────────────────────────

void EpollProxy::accept_connection() {
    // ET 모드: listen_fd에 이벤트가 한 번 오면 대기 중인 연결이 여러 개일 수 있다.
    // EAGAIN이 올 때까지 accept()를 반복해 큐를 비워야 한다.
    // 한 번만 accept하면 나머지 연결은 다음 이벤트가 오지 않아 영원히 처리 안 됨.
    while (true) {
        int client_fd = accept(listen_fd_, nullptr, nullptr);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;  // 큐 소진
            Logger::error("[EpollProxy] accept: " + std::string(strerror(errno)));
            break;
        }

        set_nonblocking(client_fd);

        int target_fd = -1;
        try {
            target_fd = connect_to_target(target_ip_, target_port_);
        } catch (const std::exception& e) {
            Logger::error("[EpollProxy] connect_to_target: " + std::string(e.what()));
            close(client_fd);
            continue;
        }
        set_nonblocking(target_fd);

        // 양방향 매핑: client_fd ↔ target_fd
        // 한쪽 fd에 이벤트가 오면 반대쪽 fd를 즉시 찾기 위해 양쪽을 키로 등록한다.
        connections_[client_fd] = { target_fd, true  };
        connections_[target_fd] = { client_fd, false };

        add_to_epoll(client_fd, EPOLLIN | EPOLLET);
        add_to_epoll(target_fd, EPOLLIN | EPOLLET);

        total_connections_++;
        active_connections_++;

        Logger::info("[EpollProxy] new connection (client_fd=" + std::to_string(client_fd) +
                     " target_fd=" + std::to_string(target_fd) +
                     ") total=" + std::to_string(total_connections_.load()));
    }
}

void EpollProxy::handle_event(int fd, uint32_t events) {
    // EPOLLERR: 소켓 에러 (네트워크 이상 등)
    // EPOLLHUP: 상대방이 연결을 닫음 (half-close 또는 RST)
    // 두 경우 모두 연결을 정리한다.
    if (events & (EPOLLERR | EPOLLHUP)) {
        close_connection(fd);
        return;
    }

    if (events & EPOLLIN) {
        auto it = connections_.find(fd);
        if (it == connections_.end()) return;  // 이미 정리된 fd
        forward_data(fd, it->second.peer_fd);
    }
}

void EpollProxy::close_connection(int fd) {
    auto it = connections_.find(fd);
    if (it == connections_.end()) return;  // 이미 정리됐거나 알 수 없는 fd

    int peer_fd = it->second.peer_fd;

    // epoll에서 제거 후 close — 순서 중요:
    // close 후에도 epoll이 이 fd를 참조하면 잘못된 이벤트가 발생할 수 있다.
    remove_from_epoll(fd);
    remove_from_epoll(peer_fd);

    close(fd);
    close(peer_fd);

    connections_.erase(fd);
    connections_.erase(peer_fd);

    active_connections_--;

    Logger::debug("[EpollProxy] connection closed (fd=" + std::to_string(fd) +
                  " peer_fd=" + std::to_string(peer_fd) + ")");
}

void EpollProxy::forward_data(int from_fd, int to_fd) {
    // splice(): 커널→유저→커널 복사 없이 fd 간 데이터를 직접 전달한다.
    //
    // read→write 방식의 문제:
    //   read(from_fd, buf)  : 커널 수신 버퍼 → 유저 공간 (복사 1회)
    //   write(to_fd, buf)   : 유저 공간 → 커널 송신 버퍼 (복사 2회)
    //
    // splice()의 데이터 경로:
    //   splice(from_fd → pipe[1]): 커널 수신 버퍼 → 파이프 버퍼 (페이지 이동)
    //   splice(pipe[0] → to_fd) : 파이프 버퍼 → 커널 송신 버퍼 (페이지 이동)
    //   파이프가 중간 버퍼 역할. 유저 공간을 거치지 않는다.
    //
    // Phase 3-B에서 파이프를 호출마다 생성하는 비용을 풀로 제거 예정.

    int pipefd[2];
    if (pipe2(pipefd, O_NONBLOCK | O_CLOEXEC) < 0) {
        Logger::error("[EpollProxy] pipe2: " + std::string(strerror(errno)));
        close_connection(from_fd);
        return;
    }

    while (true) {
        // 1단계: from_fd → pipe write end
        //   SPLICE_F_MOVE   : 페이지를 복사 대신 이동하도록 커널에 힌트
        //   SPLICE_F_NONBLOCK: 파이프 연산을 논블로킹으로
        //   65536 (64KB)    : 파이프 기본 버퍼 크기에 맞춘 청크 크기
        ssize_t n = splice(from_fd, nullptr, pipefd[1], nullptr,
                           65536, SPLICE_F_MOVE | SPLICE_F_NONBLOCK);

        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;  // 버퍼 소진, 정상
            close(pipefd[0]);
            close(pipefd[1]);
            close_connection(from_fd);
            return;
        }
        if (n == 0) {
            // EOF: 상대방이 연결을 정상 종료 (FIN)
            close(pipefd[0]);
            close(pipefd[1]);
            close_connection(from_fd);
            return;
        }

        // 2단계: pipe read end → to_fd
        // splice도 partial 전송이 가능하므로 루프 처리
        ssize_t written = 0;
        while (written < n) {
            ssize_t w = splice(pipefd[0], nullptr, to_fd, nullptr,
                               n - written, SPLICE_F_MOVE | SPLICE_F_NONBLOCK);
            if (w <= 0) {
                close(pipefd[0]);
                close(pipefd[1]);
                close_connection(from_fd);
                return;
            }
            written += w;
        }
    }

    close(pipefd[0]);
    close(pipefd[1]);
}
