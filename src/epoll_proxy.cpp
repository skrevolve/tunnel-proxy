#include "core/epoll_proxy.h"
#include "utils/logger.h"
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>      // fcntl, O_NONBLOCK
#include <unistd.h>
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
    // Phase 2-B에서 구현할 내용:
    //
    //   1. epoll_create1(EPOLL_CLOEXEC)
    //      - epoll 인스턴스를 커널에 생성하고 fd를 반환받는다.
    //      - EPOLL_CLOEXEC: exec() 계열 호출 시 자식 프로세스에 fd가 누출되지 않도록
    //        O_CLOEXEC 플래그를 설정한다. close-on-exec의 약자.
    //
    //   2. create_listening_socket(local_port)
    //      - BasicProxy와 동일하지만 set_nonblocking()을 추가로 호출한다.
    //      - ET 모드에서 블로킹 소켓을 쓰면 accept()나 read()가 무한 대기한다.
    //
    //   3. add_to_epoll(listen_fd_, EPOLLIN | EPOLLET)
    //      - listen_fd_를 epoll에 등록해 새 연결 이벤트를 감지한다.
    //      - EPOLLIN: 읽기 이벤트 (새 연결 도착 = "읽을 수 있는 상태")
    //      - EPOLLET: edge-triggered 모드 (상태 변화 시 1번만 이벤트 발생)
    (void)local_port;
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
    // Phase 2-C에서 구현할 내용:
    //
    //   running_ = true;
    //
    //   // 이벤트 배열: epoll_wait이 준비된 이벤트를 여기에 채워준다.
    //   std::vector<epoll_event> events(max_events_);
    //
    //   while (running_) {
    //       // epoll_wait(epoll_fd, 결과배열, 최대개수, 타임아웃ms)
    //       // 타임아웃 -1: I/O 이벤트가 생길 때까지 무한 대기
    //       // 타임아웃  0: 즉시 반환 (논블로킹 폴링)
    //       int n = epoll_wait(epoll_fd_, events.data(), max_events_, -1);
    //
    //       if (n < 0) {
    //           // EINTR: 시그널(SIGINT 등)로 epoll_wait가 중단됨 → 재시도
    //           if (errno == EINTR) continue;
    //           break;  // 그 외 에러는 루프 종료
    //       }
    //
    //       for (int i = 0; i < n; i++) {
    //           int fd = events[i].data.fd;
    //
    //           if (fd == listen_fd_) {
    //               // 새 클라이언트 연결 요청
    //               accept_connection();
    //           } else {
    //               // 기존 연결에서 데이터 수신 또는 에러
    //               handle_event(fd, events[i].events);
    //           }
    //       }
    //   }

    running_ = true;
    Logger::info("[EpollProxy] run() — Phase 2-B/2-C 구현 예정");
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
    // BasicProxy::create_listening_socket()과 동일하게 구현하되,
    // bind/listen 후 set_nonblocking(fd)를 호출해야 한다.
    //
    // 논블로킹이 필요한 이유:
    //   ET 모드에서 accept()가 블로킹이면, 대기 중인 연결이 없을 때
    //   영원히 블로킹되어 이벤트 루프 전체가 멈춘다.
    //   논블로킹이면 대기 연결이 없을 때 즉시 EAGAIN을 반환한다.
    (void)port;
    return -1;
}

void EpollProxy::set_nonblocking(int fd) {
    // fcntl: 파일 디스크립터 제어 (file control)
    //
    //   F_GETFL: 현재 파일 상태 플래그를 가져온다
    //   F_SETFL: 파일 상태 플래그를 설정한다
    //   O_NONBLOCK: 논블로킹 모드 활성화
    //
    // 구현 예시:
    //   int flags = fcntl(fd, F_GETFL, 0);
    //   if (flags < 0) throw std::runtime_error(...);
    //   if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) throw ...;
    //
    // 왜 F_GETFL 후 OR를 하는가:
    //   기존 플래그를 보존하면서 O_NONBLOCK만 추가하기 위해.
    //   F_SETFL에 O_NONBLOCK만 넘기면 기존 플래그(O_RDWR 등)가 날아간다.
    (void)fd;
}

// ── epoll 관리 (Phase 2-B에서 구현) ───────────────────────────────────────────

void EpollProxy::add_to_epoll(int fd, uint32_t events) {
    // epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) 구현
    //
    // epoll_event 구조체:
    //   ev.events  : 감시할 이벤트 플래그 (EPOLLIN, EPOLLET 등)
    //   ev.data.fd : 이벤트 발생 시 식별용 데이터 (fd 저장)
    //
    // 왜 ev.data.fd에 fd를 저장하는가:
    //   epoll_wait 결과에서 어떤 fd에 이벤트가 발생했는지 알기 위해.
    //   events[i].data.fd로 접근한다.
    (void)fd; (void)events;
}

void EpollProxy::mod_epoll(int fd, uint32_t events) {
    // epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) 구현
    // 이미 등록된 fd의 관심 이벤트를 변경한다.
    // 예: 쓰기 버퍼가 가득 찼을 때 EPOLLOUT을 추가
    (void)fd; (void)events;
}

void EpollProxy::remove_from_epoll(int fd) {
    // epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr) 구현
    //
    // Linux 2.6.9 이전: 마지막 인자에 nullptr을 넘길 수 없어 더미 구조체 필요
    // Linux 2.6.9 이후: nullptr 사용 가능
    // nullptr을 사용하면 코드가 간결하다.
    (void)fd;
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
