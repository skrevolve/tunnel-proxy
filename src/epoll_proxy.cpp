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

// ── 이벤트 처리 (Phase 2-C에서 구현) ──────────────────────────────────────────

void EpollProxy::accept_connection() {
    // ET 모드에서 listen_fd에 EPOLLIN 이벤트가 오면
    // 대기 중인 연결이 여러 개일 수 있다.
    // EAGAIN이 올 때까지 accept()를 반복해 모두 처리해야 한다.
    //
    // 구현 예시:
    //   while (true) {
    //       int client_fd = accept(listen_fd_, nullptr, nullptr);
    //       if (client_fd < 0) {
    //           if (errno == EAGAIN || errno == EWOULDBLOCK) break; // 다 처리함
    //           Logger::error("accept: " + std::string(strerror(errno)));
    //           break;
    //       }
    //       set_nonblocking(client_fd);
    //
    //       int target_fd = connect_to_target(target_ip_, target_port_);
    //       set_nonblocking(target_fd);
    //
    //       connections_[client_fd] = { target_fd, true  };
    //       connections_[target_fd] = { client_fd, false };
    //
    //       add_to_epoll(client_fd, EPOLLIN | EPOLLET);
    //       add_to_epoll(target_fd, EPOLLIN | EPOLLET);
    //
    //       total_connections_++;
    //       active_connections_++;
    //   }
}

void EpollProxy::handle_event(int fd, uint32_t events) {
    // 이벤트 종류별 처리:
    //
    //   EPOLLERR, EPOLLHUP: 에러 또는 연결 끊김 → close_connection()
    //   EPOLLIN            : 읽을 데이터 도착 → forward_data(fd, peer_fd)
    //
    // 구현 예시:
    //   if (events & (EPOLLERR | EPOLLHUP)) {
    //       close_connection(fd);
    //       return;
    //   }
    //   if (events & EPOLLIN) {
    //       auto it = connections_.find(fd);
    //       if (it == connections_.end()) return;  // 알 수 없는 fd
    //       forward_data(fd, it->second.peer_fd);
    //   }
    (void)fd; (void)events;
}

void EpollProxy::close_connection(int fd) {
    // 연결 종료 시 양쪽 fd를 모두 정리해야 한다.
    //
    // 구현 예시:
    //   auto it = connections_.find(fd);
    //   if (it == connections_.end()) return;
    //
    //   int peer_fd = it->second.peer_fd;
    //
    //   remove_from_epoll(fd);
    //   remove_from_epoll(peer_fd);
    //
    //   close(fd);
    //   close(peer_fd);
    //
    //   connections_.erase(fd);
    //   connections_.erase(peer_fd);
    //
    //   active_connections_--;
    (void)fd;
}

void EpollProxy::forward_data(int from_fd, int to_fd) {
    // ET 모드용 논블로킹 read/write 루프
    //
    // BasicProxy::forward_data()와의 차이:
    //   BasicProxy: 블로킹 read. 데이터가 없으면 대기.
    //   EpollProxy: 논블로킹 read. EAGAIN이 오면 즉시 반환.
    //
    // 구현 예시:
    //   char buffer[4096];
    //   while (true) {
    //       ssize_t n = read(from_fd, buffer, sizeof(buffer));
    //
    //       if (n < 0) {
    //           if (errno == EAGAIN || errno == EWOULDBLOCK) break; // 다 읽음, 루프 탈출
    //           close_connection(from_fd);  // 실제 에러
    //           return;
    //       }
    //       if (n == 0) {
    //           close_connection(from_fd);  // EOF: 상대방이 연결 종료
    //           return;
    //       }
    //
    //       // partial write 처리
    //       ssize_t written = 0;
    //       while (written < n) {
    //           ssize_t w = write(to_fd, buffer + written, n - written);
    //           if (w <= 0) { close_connection(from_fd); return; }
    //           written += w;
    //       }
    //   }
    (void)from_fd; (void)to_fd;
}
