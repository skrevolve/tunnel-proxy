#include "core/tunnel_server.h"
#include "utils/logger.h"

#include <stdexcept>
#include <system_error>
#include <cstring>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <thread>
#include <unistd.h>

namespace proxy {

// ── AgentConn 소멸자 ────────────────────────────────────────────────────────

TunnelServer::AgentConn::~AgentConn() {
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
}

// ── 생성자 / 소멸자 ─────────────────────────────────────────────────────────

TunnelServer::TunnelServer(int agent_port)
    : agent_port_(agent_port) {}

TunnelServer::~TunnelServer() {
    stop();
}

// ── 공개 API ────────────────────────────────────────────────────────────────

void TunnelServer::run() {
    agent_listen_fd_ = create_listen_socket(agent_port_);
    running_ = true;

    Logger::info("[server] listening for agents on port " +
                 std::to_string(agent_port_));

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
    } catch (const std::exception& e) {
        Logger::error("[server] send HELLO_ACK failed: " + std::string(e.what()));
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
        try {
            send_to_agent(agent_id, make_heartbeat_ack());
        } catch (const std::exception& e) {
            Logger::error("[server] send HEARTBEAT_ACK failed: " +
                          std::string(e.what()));
        }
        break;
    }

    case TunnelMsgType::OPEN_ACK: {
        // 에이전트가 OPEN을 수락 — external_fd 연결은 6-D에서 처리
        Logger::info("[server] OPEN_ACK session=" +
                     std::to_string(frame.session_id) +
                     " from " + agent_id);
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
                close_session(frame.session_id);
                return;
            }
            sent += static_cast<size_t>(s);
        }
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
            throw std::runtime_error(
                "tunnel server: send to agent " + agent_id +
                " failed: " + std::string(strerror(errno)));
        }
        sent += static_cast<size_t>(s);
    }
}

} // namespace proxy
