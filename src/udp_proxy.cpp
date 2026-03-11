#include "core/udp_proxy.h"
#include "utils/logger.h"

#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <stdexcept>
#include <cerrno>
#include <chrono>
#include <vector>

// ── 생성자 / 소멸자 ────────────────────────────────────────────────────────────

UdpProxy::UdpProxy(int local_port, const std::string& target_ip, int target_port,
                   int max_events, int session_timeout)
    : listen_fd_(-1)
    , epoll_fd_(-1)
    , target_ip_(target_ip)
    , target_port_(target_port)
    , max_events_(max_events)
    , running_(false)
    , session_timeout_(session_timeout)
    , cleanup_interval_(10)
    , last_cleanup_(Clock::now())
{
    // 1. epoll 인스턴스 생성
    epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0) {
        throw std::runtime_error("epoll_create1: " + std::string(strerror(errno)));
    }

    // 2. UDP 리스닝 소켓 생성
    listen_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (listen_fd_ < 0) {
        close(epoll_fd_);
        throw std::runtime_error("socket(SOCK_DGRAM): " + std::string(strerror(errno)));
    }

    // SO_REUSEADDR: 프로세스 재시작 시 같은 포트 즉시 재바인드 허용
    int opt = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in local_addr{};
    local_addr.sin_family      = AF_INET;
    local_addr.sin_addr.s_addr = INADDR_ANY;
    local_addr.sin_port        = htons(static_cast<uint16_t>(local_port));

    if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&local_addr), sizeof(local_addr)) < 0) {
        close(listen_fd_);
        close(epoll_fd_);
        throw std::runtime_error("bind: " + std::string(strerror(errno)));
    }

    set_nonblocking(listen_fd_);

    // 3. target_addr_ 미리 계산 (패킷마다 inet_pton 반복 방지)
    memset(&target_addr_, 0, sizeof(target_addr_));
    target_addr_.sin_family = AF_INET;
    target_addr_.sin_port   = htons(static_cast<uint16_t>(target_port_));
    if (inet_pton(AF_INET, target_ip_.c_str(), &target_addr_.sin_addr) != 1) {
        close(listen_fd_);
        close(epoll_fd_);
        throw std::runtime_error("inet_pton: invalid target IP: " + target_ip_);
    }

    // 4. listen_fd를 epoll에 등록
    add_to_epoll(listen_fd_, EPOLLIN | EPOLLET);

    Logger::info("[UdpProxy] init: local_port=" + std::to_string(local_port)
               + " target=" + target_ip_ + ":" + std::to_string(target_port_));
}

UdpProxy::~UdpProxy() {
    // 열려있는 모든 세션 정리
    std::vector<int> target_fds;
    target_fds.reserve(sessions_by_target_.size());
    for (auto& [fd, _] : sessions_by_target_) {
        target_fds.push_back(fd);
    }
    for (int fd : target_fds) {
        remove_from_epoll(fd);
        close(fd);
    }
    sessions_by_client_.clear();
    sessions_by_target_.clear();

    if (listen_fd_ >= 0) close(listen_fd_);
    if (epoll_fd_  >= 0) close(epoll_fd_);
}

// ── 이벤트 루프 ────────────────────────────────────────────────────────────────

void UdpProxy::run() {
    running_ = true;
    std::vector<epoll_event> events(max_events_);

    Logger::info("[UdpProxy] event loop started");

    while (running_) {
        int n = epoll_wait(epoll_fd_, events.data(), max_events_, 100);
        if (n < 0) {
            if (errno == EINTR) continue;
            Logger::error("[UdpProxy] epoll_wait: " + std::string(strerror(errno)));
            break;
        }

        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;
            if (fd == listen_fd_) {
                handle_client_data();
            } else {
                handle_target_data(fd);
            }
        }

        // cleanup_interval_ 마다 만료 세션 정리
        auto now = Clock::now();
        if (now - last_cleanup_ >= cleanup_interval_) {
            cleanup_expired_sessions();
            last_cleanup_ = now;
        }
    }

    Logger::info("[UdpProxy] event loop stopped");
}

void UdpProxy::stop() {
    running_ = false;
}

// ── 소켓 유틸리티 ──────────────────────────────────────────────────────────────

void UdpProxy::set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        throw std::runtime_error("fcntl F_GETFL: " + std::string(strerror(errno)));
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        throw std::runtime_error("fcntl F_SETFL O_NONBLOCK: " + std::string(strerror(errno)));
    }
}

// ── epoll 관리 ─────────────────────────────────────────────────────────────────

void UdpProxy::add_to_epoll(int fd, uint32_t events) {
    epoll_event ev{};
    ev.events   = events;
    ev.data.fd  = fd;
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
        throw std::runtime_error("epoll_ctl ADD: " + std::string(strerror(errno)));
    }
}

void UdpProxy::remove_from_epoll(int fd) {
    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
}

// ── 이벤트 처리 ────────────────────────────────────────────────────────────────

void UdpProxy::handle_client_data() {
    // ET 모드: EAGAIN까지 반복 recvfrom
    static constexpr size_t BUF_SIZE = 65536;
    char buf[BUF_SIZE];

    while (true) {
        sockaddr_in client_addr{};
        socklen_t   addr_len = sizeof(client_addr);

        ssize_t n = recvfrom(listen_fd_, buf, sizeof(buf), 0,
                             reinterpret_cast<sockaddr*>(&client_addr), &addr_len);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;  // 버퍼 소진
            Logger::error("[UdpProxy] recvfrom(listen): " + std::string(strerror(errno)));
            break;
        }
        if (n == 0) break;

        // 세션 확보 (없으면 새로 생성)
        Session* sess = get_or_create_session(client_addr);
        if (!sess) {
            Logger::error("[UdpProxy] get_or_create_session failed");
            continue;
        }

        // 타겟으로 포워딩
        //   connect()로 타겟에 바인딩된 소켓이므로 send()만으로 충분
        ssize_t sent = send(sess->target_fd, buf, static_cast<size_t>(n), 0);
        if (sent < 0) {
            Logger::error("[UdpProxy] send(target): " + std::string(strerror(errno)));
        } else {
            sess->last_activity = Clock::now();
        }
    }
}

void UdpProxy::handle_target_data(int target_fd) {
    // target_fd → 클라이언트 주소 역방향 조회
    auto it = sessions_by_target_.find(target_fd);
    if (it == sessions_by_target_.end()) {
        Logger::error("[UdpProxy] unknown target_fd: " + std::to_string(target_fd));
        close_session(target_fd);
        return;
    }

    const std::string& client_key = it->second;
    auto sit = sessions_by_client_.find(client_key);
    if (sit == sessions_by_client_.end()) {
        close_session(target_fd);
        return;
    }

    const sockaddr_in& client_addr = sit->second.client_addr;

    // ET 모드: EAGAIN까지 반복 recv
    static constexpr size_t BUF_SIZE = 65536;
    char buf[BUF_SIZE];

    while (true) {
        ssize_t n = recv(target_fd, buf, sizeof(buf), 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            Logger::error("[UdpProxy] recv(target): " + std::string(strerror(errno)));
            close_session(target_fd);
            return;
        }
        if (n == 0) {
            // UDP에서 n==0은 드물지만 방어적으로 처리
            close_session(target_fd);
            return;
        }

        // 클라이언트로 응답 포워딩
        ssize_t sent = sendto(listen_fd_, buf, static_cast<size_t>(n), 0,
                              reinterpret_cast<const sockaddr*>(&client_addr),
                              sizeof(client_addr));
        if (sent < 0) {
            Logger::error("[UdpProxy] sendto(client): " + std::string(strerror(errno)));
        } else {
            // sit 이터레이터는 위에서 find로 얻었으므로 여기서 직접 갱신
            sit->second.last_activity = Clock::now();
        }
    }
}

// ── 세션 관리 ──────────────────────────────────────────────────────────────────

UdpProxy::Session* UdpProxy::get_or_create_session(const sockaddr_in& client_addr) {
    std::string key = addr_to_key(client_addr);

    auto it = sessions_by_client_.find(key);
    if (it != sessions_by_client_.end()) {
        return &it->second;
    }

    // 새 세션 생성
    // 1. 타겟 전용 UDP 소켓 생성
    int target_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (target_fd < 0) {
        Logger::error("[UdpProxy] socket(target): " + std::string(strerror(errno)));
        return nullptr;
    }

    // 2. 타겟에 connect — 핸드셰이크 없이 커널에 "이 소켓은 이 주소와만 통신" 등록
    //    이후 send()/recv() 사용 가능, 다른 주소 패킷은 커널이 자동 필터링
    if (connect(target_fd, reinterpret_cast<const sockaddr*>(&target_addr_),
                sizeof(target_addr_)) < 0) {
        Logger::error("[UdpProxy] connect(target): " + std::string(strerror(errno)));
        close(target_fd);
        return nullptr;
    }

    // 3. 논블로킹 설정
    try {
        set_nonblocking(target_fd);
    } catch (const std::exception& e) {
        Logger::error(std::string("[UdpProxy] set_nonblocking: ") + e.what());
        close(target_fd);
        return nullptr;
    }

    // 4. epoll 등록
    try {
        add_to_epoll(target_fd, EPOLLIN | EPOLLET);
    } catch (const std::exception& e) {
        Logger::error(std::string("[UdpProxy] add_to_epoll: ") + e.what());
        close(target_fd);
        return nullptr;
    }

    // 5. 세션 테이블 등록
    Session sess{ target_fd, client_addr, Clock::now() };
    sessions_by_client_[key]        = sess;
    sessions_by_target_[target_fd]  = key;

    Logger::info("[UdpProxy] new session: " + key + " → target_fd=" + std::to_string(target_fd));

    return &sessions_by_client_[key];
}

void UdpProxy::cleanup_expired_sessions() {
    auto now = Clock::now();

    // 이터레이터 무효화를 피하기 위해 만료 fd 목록을 먼저 수집
    std::vector<int> expired;
    for (auto& [fd, key] : sessions_by_target_) {
        auto it = sessions_by_client_.find(key);
        if (it == sessions_by_client_.end()) {
            expired.push_back(fd);
            continue;
        }
        if (now - it->second.last_activity >= session_timeout_) {
            expired.push_back(fd);
        }
    }

    for (int fd : expired) {
        Logger::info("[UdpProxy] session timeout: fd=" + std::to_string(fd));
        close_session(fd);
    }
}

void UdpProxy::close_session(int target_fd) {
    auto it = sessions_by_target_.find(target_fd);
    if (it == sessions_by_target_.end()) return;

    const std::string key = it->second;

    remove_from_epoll(target_fd);
    close(target_fd);

    sessions_by_client_.erase(key);
    sessions_by_target_.erase(target_fd);

    Logger::info("[UdpProxy] session closed: " + key);
}

// ── 유틸리티 ───────────────────────────────────────────────────────────────────

std::string UdpProxy::addr_to_key(const sockaddr_in& addr) {
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
    return std::string(ip) + ":" + std::to_string(ntohs(addr.sin_port));
}
