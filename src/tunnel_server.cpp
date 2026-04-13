#include "core/tunnel_server.h"
#include "utils/logger.h"

#include <stdexcept>
#include <system_error>
#include <cstring>
#include <chrono>

#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <thread>
#include <unistd.h>

namespace proxy {

// ── AgentConn 생성자 / 소멸자 ───────────────────────────────────────────────

TunnelServer::AgentConn::AgentConn(int f, std::string id)
    : fd(f), agent_id(std::move(id)),
      last_heartbeat_ns(
          std::chrono::steady_clock::now().time_since_epoch().count()) {}

TunnelServer::AgentConn::~AgentConn() {
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
}

// ── 생성자 / 소멸자 ─────────────────────────────────────────────────────────

TunnelServer::TunnelServer(int agent_port, int proxy_port, int agent_timeout_s)
    : agent_port_(agent_port), proxy_port_(proxy_port),
      agent_timeout_s_(agent_timeout_s) {}

TunnelServer::~TunnelServer() {
    stop();
}

// ── 공개 API ────────────────────────────────────────────────────────────────

void TunnelServer::run() {
    agent_listen_fd_ = create_listen_socket(agent_port_);
    running_ = true;

    Logger::info("[server] listening for agents on port " +
                 std::to_string(agent_port_));

    // 외부 클라이언트 리스너 시작 (proxy_port > 0인 경우)
    if (proxy_port_ > 0) {
        proxy_listen_fd_ = create_listen_socket(proxy_port_);
        proxy_listener_thread_ = std::thread(&TunnelServer::proxy_accept_loop, this);
        Logger::info("[server] listening for external clients on port " +
                     std::to_string(proxy_port_));
    }

    // watchdog 시작 (agent_timeout_s_ > 0인 경우)
    if (agent_timeout_s_ > 0) {
        watchdog_thread_ = std::thread(&TunnelServer::watchdog_loop, this);
        Logger::info("[server] agent watchdog started (timeout=" +
                     std::to_string(agent_timeout_s_) + "s)");
    }

    while (running_) {
        sockaddr_in client_addr{};
        socklen_t   addr_len = sizeof(client_addr);

        int agent_fd = accept(agent_listen_fd_,
                              reinterpret_cast<sockaddr*>(&client_addr),
                              &addr_len);
        if (agent_fd < 0) {
            if (!running_) break;
            Logger::error("[server] accept failed: " +
                          std::string(strerror(errno)));
            continue;
        }

        char ip_buf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip_buf, sizeof(ip_buf));
        Logger::info("[server] new agent connection from " +
                     std::string(ip_buf) + ":" +
                     std::to_string(ntohs(client_addr.sin_port)));

        // 각 에이전트 연결을 detached thread로 처리
        std::thread(&TunnelServer::handle_agent_connection, this, agent_fd)
            .detach();
    }

    stop();
}

void TunnelServer::stop() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) {
        return;  // 이미 중지됨
    }

    // 리스닝 소켓 닫기 → accept() 탈출
    if (agent_listen_fd_ >= 0) {
        shutdown(agent_listen_fd_, SHUT_RDWR);
        close(agent_listen_fd_);
        agent_listen_fd_ = -1;
    }

    if (proxy_listen_fd_ >= 0) {
        shutdown(proxy_listen_fd_, SHUT_RDWR);
        close(proxy_listen_fd_);
        proxy_listen_fd_ = -1;
    }

    if (proxy_listener_thread_.joinable()) {
        proxy_listener_thread_.join();
    }

    if (watchdog_thread_.joinable()) {
        watchdog_thread_.join();
    }

    // pending OPEN_ACK 대기 스레드들에게 예외 신호
    {
        std::lock_guard<std::mutex> lock(pending_open_mutex_);
        for (auto& [sid, p] : pending_open_) {
            try { p.set_exception(
                std::make_exception_ptr(
                    std::runtime_error("server stopped"))); } catch (...) {}
        }
        pending_open_.clear();
    }

    // 모든 에이전트 연결 종료 → per-agent thread의 recv 탈출
    std::unordered_map<std::string, std::shared_ptr<AgentConn>> agents_copy;
    {
        std::lock_guard<std::mutex> lock(agents_mutex_);
        agents_copy = agents_;
        agents_.clear();
    }

    for (auto& [id, conn] : agents_copy) {
        if (conn->fd >= 0) {
            shutdown(conn->fd, SHUT_RDWR);
            close(conn->fd);
            conn->fd = -1;
        }
    }

    // 세션 맵 초기화
    std::lock_guard<std::mutex> slock(sessions_mutex_);
    sessions_.clear();
}

uint32_t TunnelServer::open_session(const std::string& agent_id,
                                    const std::string& target_ip,
                                    uint16_t target_port) {
    // 에이전트가 등록되어 있는지 확인
    {
        std::lock_guard<std::mutex> lock(agents_mutex_);
        if (agents_.find(agent_id) == agents_.end()) {
            Logger::warning("[server] open_session: agent not found: " + agent_id);
            return 0;
        }
    }

    uint32_t session_id = generate_session_id();

    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        sessions_[session_id] = Session{session_id, agent_id, -1};
    }

    try {
        send_to_agent(agent_id, make_open(session_id, target_ip, target_port));
        Logger::info("[server] OPEN session=" + std::to_string(session_id) +
                     " agent=" + agent_id +
                     " target=" + target_ip + ":" + std::to_string(target_port));
    } catch (const std::exception& e) {
        Logger::error("[server] send OPEN failed: " + std::string(e.what()));
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        sessions_.erase(session_id);
        return 0;
    }

    return session_id;
}

bool TunnelServer::set_session_external_fd(uint32_t session_id, int external_fd) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) return false;
    it->second.external_fd = external_fd;
    return true;
}

uint32_t TunnelServer::get_agent_count() const {
    std::lock_guard<std::mutex> lock(agents_mutex_);
    return static_cast<uint32_t>(agents_.size());
}

uint32_t TunnelServer::get_session_count() const {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    return static_cast<uint32_t>(sessions_.size());
}

std::vector<std::string> TunnelServer::get_agent_ids() const {
    std::lock_guard<std::mutex> lock(agents_mutex_);
    std::vector<std::string> ids;
    ids.reserve(agents_.size());
    for (const auto& [id, _] : agents_) {
        ids.push_back(id);
    }
    return ids;
}

// ── 내부 함수 ───────────────────────────────────────────────────────────────

int TunnelServer::create_listen_socket(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        throw std::system_error(errno, std::generic_category(), "socket");
    }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        int err = errno;
        close(fd);
        throw std::system_error(err, std::generic_category(),
                                "bind port " + std::to_string(port));
    }

    if (listen(fd, SOMAXCONN) < 0) {
        int err = errno;
        close(fd);
        throw std::system_error(err, std::generic_category(), "listen");
    }

    return fd;
}

void TunnelServer::handle_agent_connection(int agent_fd) {
    // ── HELLO 수신 ──────────────────────────────────────────────────────────
    TunnelFrame hello_frame;
    if (!recv_frame(agent_fd, hello_frame) ||
        hello_frame.type != TunnelMsgType::HELLO) {
        Logger::warning("[server] expected HELLO, got unexpected frame or disconnect");
        close(agent_fd);
        return;
    }

    std::string agent_id;
    try {
        agent_id = parse_hello_payload(hello_frame.payload);
    } catch (const std::exception& e) {
        Logger::error("[server] HELLO parse error: " + std::string(e.what()));
        close(agent_fd);
        return;
    }

    Logger::info("[server] agent registered: " + agent_id);
    metrics_.record_connection_attempt();

    // ── 에이전트 등록 ────────────────────────────────────────────────────────
    {
        std::lock_guard<std::mutex> lock(agents_mutex_);
        // 동일 agent_id가 이미 있으면 이전 연결 교체
        auto it = agents_.find(agent_id);
        if (it != agents_.end()) {
            Logger::warning("[server] agent " + agent_id +
                            " reconnected — replacing old connection");
            if (it->second->fd >= 0) {
                shutdown(it->second->fd, SHUT_RDWR);
                close(it->second->fd);
                it->second->fd = -1;
            }
        }
        agents_[agent_id] = std::make_shared<AgentConn>(agent_fd, agent_id);
    }

    // ── HELLO_ACK 송신 ───────────────────────────────────────────────────────
    try {
        send_to_agent(agent_id, make_hello_ack());
        Logger::info("[server] sent HELLO_ACK to " + agent_id);
        metrics_.record_connection_success();
    } catch (const std::exception& e) {
        Logger::error("[server] send HELLO_ACK failed: " + std::string(e.what()));
        metrics_.record_connection_failure();
        metrics_.record_error();
        unregister_agent(agent_id);
        return;
    }

    // ── 프레임 수신 루프 ─────────────────────────────────────────────────────
    TunnelFrame frame;
    while (running_ && recv_frame(agent_fd, frame)) {
        handle_agent_frame(agent_id, frame);
    }

    Logger::info("[server] agent " + agent_id + " disconnected");
    unregister_agent(agent_id);
}

void TunnelServer::handle_agent_frame(const std::string& agent_id,
                                      const TunnelFrame& frame) {
    switch (frame.type) {

    case TunnelMsgType::HEARTBEAT: {
        Logger::info("[server] HEARTBEAT from " + agent_id +
                     " — sending ACK");
        // last_heartbeat_ns 갱신 (watchdog 타임아웃 리셋)
        {
            std::lock_guard<std::mutex> lock(agents_mutex_);
            auto it = agents_.find(agent_id);
            if (it != agents_.end()) {
                it->second->last_heartbeat_ns.store(
                    std::chrono::steady_clock::now().time_since_epoch().count());
            }
        }
        try {
            send_to_agent(agent_id, make_heartbeat_ack());
        } catch (const std::exception& e) {
            Logger::error("[server] send HEARTBEAT_ACK failed: " +
                          std::string(e.what()));
        }
        break;
    }

    case TunnelMsgType::OPEN_ACK: {
        Logger::info("[server] OPEN_ACK session=" +
                     std::to_string(frame.session_id) +
                     " from " + agent_id);
        // 대기 중인 external client thread에 시그널
        {
            std::lock_guard<std::mutex> lock(pending_open_mutex_);
            auto it = pending_open_.find(frame.session_id);
            if (it != pending_open_.end()) {
                try { it->second.set_value(); } catch (...) {}
                pending_open_.erase(it);
            }
        }
        break;
    }

    case TunnelMsgType::DATA: {
        // 에이전트 → 외부 클라이언트 데이터 포워딩
        int external_fd = -1;
        {
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            auto it = sessions_.find(frame.session_id);
            if (it != sessions_.end()) {
                external_fd = it->second.external_fd;
            }
        }

        if (external_fd < 0) {
            // external_fd 미설정 — Phase 6-D 완료 전 정상 상황
            Logger::debug("[server] DATA session=" +
                          std::to_string(frame.session_id) +
                          " — external_fd not yet set (6-D pending)");
            break;
        }

        // external_fd로 포워딩 (Phase 6-D에서 활성화)
        size_t sent = 0;
        while (sent < frame.payload.size()) {
            ssize_t s = send(external_fd,
                             frame.payload.data() + sent,
                             frame.payload.size() - sent, MSG_NOSIGNAL);
            if (s <= 0) {
                Logger::warning("[server] forward to external failed, "
                                "closing session " +
                                std::to_string(frame.session_id));
                metrics_.record_error();
                close_session(frame.session_id);
                return;
            }
            sent += static_cast<size_t>(s);
        }
        metrics_.record_bytes_received(frame.payload.size());
        break;
    }

    case TunnelMsgType::CLOSE: {
        Logger::info("[server] CLOSE session=" +
                     std::to_string(frame.session_id) +
                     " from " + agent_id);
        close_session(frame.session_id);
        break;
    }

    default:
        Logger::warning("[server] unexpected frame type " +
                        std::to_string(static_cast<int>(frame.type)) +
                        " from " + agent_id);
        break;
    }
}

void TunnelServer::unregister_agent(const std::string& agent_id) {
    // agents_ 맵에서 제거
    {
        std::lock_guard<std::mutex> lock(agents_mutex_);
        agents_.erase(agent_id);
    }

    // 이 에이전트에 속한 세션 전체 정리
    std::vector<uint32_t> sessions_to_close;
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        for (const auto& [sid, session] : sessions_) {
            if (session.agent_id == agent_id) {
                sessions_to_close.push_back(sid);
            }
        }
        for (uint32_t sid : sessions_to_close) {
            sessions_.erase(sid);
        }
    }

    if (!sessions_to_close.empty()) {
        Logger::info("[server] agent " + agent_id + " removed " +
                     std::to_string(sessions_to_close.size()) +
                     " session(s)");
    }
}

void TunnelServer::close_session(uint32_t session_id) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    sessions_.erase(session_id);
}

uint32_t TunnelServer::generate_session_id() {
    // session_id=0은 컨트롤 채널 예약 → 0이 나오면 건너뜀
    uint32_t id;
    do {
        id = next_session_id_.fetch_add(1, std::memory_order_relaxed);
    } while (id == 0);
    return id;
}

// ── 수신 유틸리티 ───────────────────────────────────────────────────────────

bool TunnelServer::recv_exact(int fd, uint8_t* buf, size_t n) {
    size_t received = 0;
    while (received < n) {
        ssize_t r = recv(fd, buf + received, n - received, 0);
        if (r <= 0) return false;
        received += static_cast<size_t>(r);
    }
    return true;
}

bool TunnelServer::recv_frame(int fd, TunnelFrame& out) {
    uint8_t header_buf[TUNNEL_HEADER_SIZE];
    if (!recv_exact(fd, header_buf, TUNNEL_HEADER_SIZE)) {
        return false;
    }

    TunnelFrame frame;
    try {
        frame = parse_header(header_buf, TUNNEL_HEADER_SIZE);
    } catch (const std::exception& e) {
        Logger::error("[server] parse_header: " + std::string(e.what()));
        return false;
    }

    if (frame.length > 0) {
        frame.payload.resize(frame.length);
        if (!recv_exact(fd, frame.payload.data(), frame.length)) {
            return false;
        }
    }

    out = std::move(frame);
    return true;
}

void TunnelServer::send_to_agent(const std::string& agent_id,
                                 const TunnelFrame& frame) {
    // agents_ 락 최소화: shared_ptr을 꺼낸 뒤 락 해제
    std::shared_ptr<AgentConn> conn;
    {
        std::lock_guard<std::mutex> lock(agents_mutex_);
        auto it = agents_.find(agent_id);
        if (it == agents_.end()) return;
        conn = it->second;
    }

    auto buf = serialize(frame);
    std::lock_guard<std::mutex> send_lock(conn->send_mutex);

    if (conn->fd < 0) return;

    size_t sent = 0;
    while (sent < buf.size()) {
        ssize_t s = send(conn->fd, buf.data() + sent,
                         buf.size() - sent, MSG_NOSIGNAL);
        if (s <= 0) {
            metrics_.record_error();
            throw std::runtime_error(
                "tunnel server: send to agent " + agent_id +
                " failed: " + std::string(strerror(errno)));
        }
        sent += static_cast<size_t>(s);
    }
    if (frame.type == TunnelMsgType::DATA) {
        metrics_.record_bytes_sent(frame.payload.size());
    }
}

// ── Phase 11-A: 에이전트 heartbeat watchdog ─────────────────────────────────

void TunnelServer::watchdog_loop() {
    constexpr int CHECK_INTERVAL_S = 10;
    const int64_t timeout_ns =
        static_cast<int64_t>(agent_timeout_s_) * 1'000'000'000LL;

    for (int elapsed = 0; running_; ++elapsed) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (elapsed < CHECK_INTERVAL_S) continue;
        elapsed = 0;

        auto now = std::chrono::steady_clock::now().time_since_epoch().count();

        // 타임아웃 에이전트 fd 목록 수집
        std::vector<std::pair<std::string, int>> timed_out;
        {
            std::lock_guard<std::mutex> lock(agents_mutex_);
            for (auto& [id, conn] : agents_) {
                int64_t last = conn->last_heartbeat_ns.load();
                if ((now - last) > timeout_ns) {
                    timed_out.emplace_back(id, conn->fd);
                }
            }
        }

        for (auto& [id, fd] : timed_out) {
            Logger::warning("[server] agent " + id +
                            " heartbeat timeout (" +
                            std::to_string(agent_timeout_s_) +
                            "s) — closing connection");
            // fd shutdown → per-agent thread의 recv_frame 탈출 → unregister_agent 호출
            if (fd >= 0) {
                shutdown(fd, SHUT_RDWR);
            }
        }
    }
    Logger::info("[server] watchdog thread exited");
}

// ── Phase 14-B: Guacamole 게이트웨이 직접 터널 연결 ────────────────────────

int TunnelServer::connect_via_tunnel(const std::string& agent_id,
                                     const std::string& target_ip,
                                     uint16_t target_port) {
    // socketpair: fds[0] = 서버 측 (external_fd), fds[1] = 호출자 측 (curl용)
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0) {
        Logger::error("[server] connect_via_tunnel socketpair failed: " +
                      std::string(strerror(errno)));
        return -1;
    }

    // OPEN_ACK 대기 설정
    std::promise<void> ack_promise;
    auto ack_future = ack_promise.get_future();

    uint32_t session_id = open_session(agent_id, target_ip, target_port);
    if (session_id == 0) {
        Logger::error("[server] connect_via_tunnel: open_session failed for agent=" + agent_id);
        close(fds[0]); close(fds[1]);
        return -1;
    }

    {
        std::lock_guard<std::mutex> lock(pending_open_mutex_);
        pending_open_[session_id] = std::move(ack_promise);
    }

    // OPEN_ACK 대기 (에이전트가 내부 서버에 연결할 때까지)
    auto status = ack_future.wait_for(std::chrono::seconds(OPEN_ACK_TIMEOUT_S));
    if (status != std::future_status::ready) {
        Logger::error("[server] connect_via_tunnel: OPEN_ACK timeout, session=" +
                      std::to_string(session_id));
        {
            std::lock_guard<std::mutex> lock(pending_open_mutex_);
            pending_open_.erase(session_id);
        }
        close_session(session_id);
        close(fds[0]); close(fds[1]);
        return -1;
    }

    try { ack_future.get(); } catch (...) {
        close_session(session_id);
        close(fds[0]); close(fds[1]);
        return -1;
    }

    // fds[0]를 세션 external_fd로 등록
    // handle_agent_frame(DATA)가 send(fds[0], ...) 호출 → fds[1]에서 읽힘 (curl 수신 방향)
    if (!set_session_external_fd(session_id, fds[0])) {
        Logger::error("[server] connect_via_tunnel: session gone, session=" +
                      std::to_string(session_id));
        close(fds[0]); close(fds[1]);
        return -1;
    }

    // 중계 스레드: fds[0]에서 읽기 (curl이 fds[1]에 쓴 HTTP 요청)
    //             → DATA 프레임으로 에이전트에 전송 (curl→내부서버 방향)
    std::thread([this, session_id, agent_id, relay_fd = fds[0]]() {
        constexpr size_t BUF_SIZE = 4096;
        uint8_t buf[BUF_SIZE];

        while (running_) {
            ssize_t n = recv(relay_fd, buf, BUF_SIZE, 0);
            if (n <= 0) break;

            std::vector<uint8_t> data(buf, buf + static_cast<size_t>(n));
            try {
                send_to_agent(agent_id, make_data(session_id, std::move(data)));
            } catch (...) {
                break;
            }
        }

        // 정리: CLOSE 전송 + 세션/fd 해제
        try { send_to_agent(agent_id, make_close(session_id)); } catch (...) {}
        close_session(session_id);
        close(relay_fd);
        Logger::info("[server] tunnel relay closed, session=" +
                     std::to_string(session_id));
    }).detach();

    Logger::info("[server] connect_via_tunnel established: session=" +
                 std::to_string(session_id) + " agent=" + agent_id +
                 " target=" + target_ip + ":" + std::to_string(target_port));

    return fds[1];  // 호출자(curl)가 사용하는 fd
}

// ── Phase 6-D: 외부 클라이언트 포워딩 ──────────────────────────────────────

void TunnelServer::set_forward_target(const std::string& agent_id,
                                      const std::string& target_ip,
                                      uint16_t target_port) {
    std::lock_guard<std::mutex> lock(forward_target_mutex_);
    forward_target_ = ForwardTarget{agent_id, target_ip, target_port};
    Logger::info("[server] forward target set: agent=" + agent_id +
                 " target=" + target_ip + ":" + std::to_string(target_port));
}

void TunnelServer::proxy_accept_loop() {
    while (running_) {
        sockaddr_in client_addr{};
        socklen_t   addr_len = sizeof(client_addr);

        int external_fd = accept(proxy_listen_fd_,
                                 reinterpret_cast<sockaddr*>(&client_addr),
                                 &addr_len);
        if (external_fd < 0) {
            if (!running_) break;
            Logger::error("[server] proxy accept failed: " +
                          std::string(strerror(errno)));
            continue;
        }

        char ip_buf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip_buf, sizeof(ip_buf));
        Logger::info("[server] external client from " +
                     std::string(ip_buf) + ":" +
                     std::to_string(ntohs(client_addr.sin_port)));

        std::thread(&TunnelServer::handle_external_connection, this, external_fd)
            .detach();
    }
}

void TunnelServer::handle_external_connection(int external_fd) {
    // ── ForwardTarget 조회 ──────────────────────────────────────────────────
    std::string agent_id, target_ip;
    uint16_t    target_port;
    {
        std::lock_guard<std::mutex> lock(forward_target_mutex_);
        if (!forward_target_) {
            Logger::warning("[server] no forward target configured, "
                            "rejecting external connection");
            close(external_fd);
            return;
        }
        agent_id    = forward_target_->agent_id;
        target_ip   = forward_target_->target_ip;
        target_port = forward_target_->target_port;
    }

    // ── 세션 개설 + OPEN_ACK 대기 ───────────────────────────────────────────
    std::promise<void> ack_promise;
    auto ack_future = ack_promise.get_future();

    uint32_t session_id = open_session(agent_id, target_ip, target_port);
    if (session_id == 0) {
        Logger::error("[server] open_session failed for agent " + agent_id);
        close(external_fd);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(pending_open_mutex_);
        pending_open_[session_id] = std::move(ack_promise);
    }

    // 에이전트가 내부 서버에 연결할 때까지 대기 (최대 OPEN_ACK_TIMEOUT_S 초)
    auto status = ack_future.wait_for(
        std::chrono::seconds(OPEN_ACK_TIMEOUT_S));

    if (status != std::future_status::ready) {
        Logger::error("[server] OPEN_ACK timeout, session=" +
                      std::to_string(session_id));
        {
            std::lock_guard<std::mutex> lock(pending_open_mutex_);
            pending_open_.erase(session_id);
        }
        close_session(session_id);
        close(external_fd);
        return;
    }

    // future에서 예외 전파 확인 (stop() 호출 등)
    try {
        ack_future.get();
    } catch (...) {
        close(external_fd);
        return;
    }

    // ── external_fd → session 연결 ──────────────────────────────────────────
    if (!set_session_external_fd(session_id, external_fd)) {
        Logger::error("[server] session gone before external_fd could be set, "
                      "session=" + std::to_string(session_id));
        close(external_fd);
        return;
    }

    Logger::info("[server] session=" + std::to_string(session_id) +
                 " fully established, forwarding started");

    // ── 외부 클라이언트 → 에이전트 포워딩 루프 ─────────────────────────────
    //   에이전트 → 외부 클라이언트 방향은 handle_agent_frame(DATA)에서 처리.
    constexpr size_t BUF_SIZE = 4096;
    uint8_t buf[BUF_SIZE];

    while (running_) {
        ssize_t n = recv(external_fd, buf, BUF_SIZE, 0);
        if (n <= 0) break;

        std::vector<uint8_t> data(buf, buf + n);
        try {
            send_to_agent(agent_id,
                          make_data(session_id, std::move(data)));
        } catch (const std::exception& e) {
            Logger::error("[server] forward to agent failed: " +
                          std::string(e.what()));
            break;
        }
    }

    // ── 정리 ────────────────────────────────────────────────────────────────
    Logger::info("[server] external client closed, session=" +
                 std::to_string(session_id));

    try {
        send_to_agent(agent_id, make_close(session_id));
    } catch (...) {}

    close_session(session_id);
    close(external_fd);
}

} // namespace proxy
