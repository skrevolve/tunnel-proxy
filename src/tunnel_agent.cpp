#include "core/tunnel_agent.h"
#include "utils/logger.h"

#include <stdexcept>
#include <system_error>
#include <cstring>
#include <chrono>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

namespace proxy {

// ── Session 소멸자 ──────────────────────────────────────────────────────────

TunnelAgent::Session::~Session() {
    if (target_fd >= 0) {
        close(target_fd);
    }
}

// ── 생성자 / 소멸자 ─────────────────────────────────────────────────────────

TunnelAgent::TunnelAgent(const std::string& server_ip, int server_port,
                         const std::string& agent_id,
                         int heartbeat_interval_s,
                         int heartbeat_timeout_s)
    : server_ip_(server_ip), server_port_(server_port),
      agent_id_(agent_id), heartbeat_interval_s_(heartbeat_interval_s),
      heartbeat_timeout_s_(heartbeat_timeout_s > 0
                           ? heartbeat_timeout_s
                           : heartbeat_interval_s * 3) {}

TunnelAgent::~TunnelAgent() {
    stop();
}

// ── 공개 API ────────────────────────────────────────────────────────────────

void TunnelAgent::run() {
    running_ = true;
    int delay_s = 1;

    while (running_) {
        try {
            connect_and_run();
        } catch (const std::exception& e) {
            if (!running_) break;
            Logger::warning("[agent] connection error: " +
                            std::string(e.what()));
        }

        if (!running_) break;

        // 지수 백오프 대기: 1s → 2s → 4s → ... → MAX_RECONNECT_DELAY_S(60s)
        Logger::info("[agent] reconnecting in " +
                     std::to_string(delay_s) + "s ...");
        current_reconnect_delay_.store(delay_s);

        for (int i = 0; i < delay_s && running_; ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        delay_s = std::min(delay_s * 2, MAX_RECONNECT_DELAY_S);
    }

    current_reconnect_delay_.store(0);
    Logger::info("[agent] stopped");
}

void TunnelAgent::connect_and_run() {
    server_fd_ = connect_to_server();
    Logger::info("[agent] connected to server " + server_ip_ + ":" +
                 std::to_string(server_port_));

    // ── HELLO 교환 ──────────────────────────────────────────────────────────
    send_frame(make_hello(agent_id_));
    Logger::info("[agent] sent HELLO (agent_id=" + agent_id_ + ")");

    TunnelFrame resp;
    if (!recv_frame(server_fd_, resp) ||
        resp.type != TunnelMsgType::HELLO_ACK) {
        close(server_fd_);
        server_fd_ = -1;
        throw std::runtime_error("tunnel agent: HELLO_ACK not received");
    }
    Logger::info("[agent] received HELLO_ACK — registered");

    // ── HEARTBEAT 스레드 시작 ───────────────────────────────────────────────
    // last_ack_ns_ 초기화: 연결 직후를 기준으로 타임아웃 카운트 시작
    last_ack_ns_.store(
        std::chrono::steady_clock::now().time_since_epoch().count());
    heartbeat_thread_ = std::thread(&TunnelAgent::heartbeat_loop, this);

    // ── 서버 프레임 수신 루프 ──────────────────────────────────────────────
    TunnelFrame frame;
    while (running_ && recv_frame(server_fd_, frame)) {
        switch (frame.type) {
            case TunnelMsgType::OPEN:
                handle_open(frame);
                break;
            case TunnelMsgType::DATA:
                handle_data(frame);
                break;
            case TunnelMsgType::CLOSE:
                handle_close(frame);
                break;
            case TunnelMsgType::HEARTBEAT_ACK:
                last_ack_ns_.store(
                    std::chrono::steady_clock::now().time_since_epoch().count());
                Logger::info("[agent] HEARTBEAT_ACK received");
                break;
            default:
                Logger::warning("[agent] unexpected frame type: " +
                                std::to_string(static_cast<int>(frame.type)));
                break;
        }
    }

    Logger::info("[agent] server connection closed");
    cleanup_connection();
    // 재연결 대기 후 run()이 다시 connect_and_run()을 호출
}

void TunnelAgent::stop() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) {
        return;  // 이미 중지됨
    }

    // server_fd shutdown → recv_frame 탈출 + 재연결 백오프 루프 깨우기
    if (server_fd_ >= 0) {
        shutdown(server_fd_, SHUT_RDWR);
    }

    cleanup_connection();
}

void TunnelAgent::cleanup_connection() {
    // 모든 세션 target_fd 닫기 → per-session 스레드 종료 유도
    std::unordered_map<uint32_t, std::unique_ptr<Session>> sessions_copy;
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        sessions_copy = std::move(sessions_);
    }

    for (auto& [sid, session] : sessions_copy) {
        if (session->target_fd >= 0) {
            shutdown(session->target_fd, SHUT_RDWR);
            close(session->target_fd);
            session->target_fd = -1;
        }
        if (session->reader.joinable()) {
            session->reader.detach();
        }
    }

    // heartbeat 스레드 종료 대기
    if (heartbeat_thread_.joinable()) {
        heartbeat_thread_.join();
    }

    if (server_fd_ >= 0) {
        close(server_fd_);
        server_fd_ = -1;
    }
}

uint32_t TunnelAgent::get_active_sessions() const {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    return static_cast<uint32_t>(sessions_.size());
}

int64_t TunnelAgent::seconds_since_last_ack() const {
    int64_t last = last_ack_ns_.load();
    if (last == 0) return 0;
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return (now - last) / 1'000'000'000LL;
}

int TunnelAgent::current_reconnect_delay() const {
    return current_reconnect_delay_.load();
}

// ── 네트워크 유틸리티 ───────────────────────────────────────────────────────

int TunnelAgent::connect_to_server() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        throw std::system_error(errno, std::generic_category(), "socket");
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(static_cast<uint16_t>(server_port_));

    if (inet_pton(AF_INET, server_ip_.c_str(), &addr.sin_addr) != 1) {
        close(fd);
        throw std::runtime_error("tunnel agent: invalid server IP: " + server_ip_);
    }

    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        int err = errno;
        close(fd);
        throw std::system_error(err, std::generic_category(),
                                "connect to " + server_ip_ + ":" +
                                std::to_string(server_port_));
    }

    return fd;
}

int TunnelAgent::connect_to_target(const std::string& ip, uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        throw std::system_error(errno, std::generic_category(), "socket");
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);

    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
        close(fd);
        throw std::runtime_error("tunnel agent: invalid target IP: " + ip);
    }

    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        int err = errno;
        close(fd);
        throw std::system_error(err, std::generic_category(),
                                "connect to target " + ip + ":" +
                                std::to_string(port));
    }

    return fd;
}

bool TunnelAgent::recv_exact(int fd, uint8_t* buf, size_t n) {
    size_t received = 0;
    while (received < n) {
        ssize_t r = recv(fd, buf + received, n - received, 0);
        if (r <= 0) return false;
        received += static_cast<size_t>(r);
    }
    return true;
}

bool TunnelAgent::recv_frame(int fd, TunnelFrame& out) {
    uint8_t header_buf[TUNNEL_HEADER_SIZE];
    if (!recv_exact(fd, header_buf, TUNNEL_HEADER_SIZE)) {
        return false;
    }

    TunnelFrame frame;
    try {
        frame = parse_header(header_buf, TUNNEL_HEADER_SIZE);
    } catch (const std::exception& e) {
        Logger::error("[agent] parse_header: " + std::string(e.what()));
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

void TunnelAgent::send_frame(const TunnelFrame& frame) {
    auto buf = serialize(frame);
    std::lock_guard<std::mutex> lock(send_mutex_);
    size_t sent = 0;
    while (sent < buf.size()) {
        ssize_t s = send(server_fd_, buf.data() + sent,
                         buf.size() - sent, MSG_NOSIGNAL);
        if (s <= 0) {
            throw std::runtime_error(
                "tunnel agent: send failed: " + std::string(strerror(errno)));
        }
        sent += static_cast<size_t>(s);
    }
}

// ── 프레임 핸들러 ───────────────────────────────────────────────────────────

void TunnelAgent::handle_open(const TunnelFrame& frame) {
    std::string target_ip;
    uint16_t    target_port;

    try {
        auto [ip, port]  = parse_open_payload(frame.payload);
        target_ip   = ip;
        target_port = port;
    } catch (const std::exception& e) {
        Logger::error("[agent] OPEN payload parse error: " + std::string(e.what()));
        return;
    }

    Logger::info("[agent] OPEN session=" + std::to_string(frame.session_id) +
                 " target=" + target_ip + ":" + std::to_string(target_port));

    int target_fd;
    try {
        target_fd = connect_to_target(target_ip, target_port);
    } catch (const std::exception& e) {
        Logger::error("[agent] connect to target failed: " + std::string(e.what()));
        try { send_frame(make_close(frame.session_id)); } catch (...) {}
        return;
    }

    auto session      = std::make_unique<Session>(target_fd);
    const uint32_t sid = frame.session_id;
    session->reader   = std::thread(&TunnelAgent::target_reader, this, sid, target_fd);

    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        sessions_[sid] = std::move(session);
    }

    try {
        send_frame(make_open_ack(sid));
    } catch (const std::exception& e) {
        Logger::error("[agent] send OPEN_ACK failed: " + std::string(e.what()));
        close_session(sid);
        return;
    }

    Logger::info("[agent] session " + std::to_string(sid) + " established");
}

void TunnelAgent::handle_data(const TunnelFrame& frame) {
    int target_fd = -1;
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        auto it = sessions_.find(frame.session_id);
        if (it == sessions_.end()) {
            Logger::warning("[agent] DATA for unknown session " +
                            std::to_string(frame.session_id));
            return;
        }
        target_fd = it->second->target_fd;
    }

    size_t sent = 0;
    while (sent < frame.payload.size()) {
        ssize_t s = send(target_fd,
                         frame.payload.data() + sent,
                         frame.payload.size() - sent, MSG_NOSIGNAL);
        if (s <= 0) {
            Logger::warning("[agent] write to target failed, closing session " +
                            std::to_string(frame.session_id));
            close_session(frame.session_id);
            return;
        }
        sent += static_cast<size_t>(s);
    }
}

void TunnelAgent::handle_close(const TunnelFrame& frame) {
    Logger::info("[agent] CLOSE session=" + std::to_string(frame.session_id));
    close_session(frame.session_id);
}

void TunnelAgent::close_session(uint32_t session_id) {
    std::unique_ptr<Session> session;
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        auto it = sessions_.find(session_id);
        if (it == sessions_.end()) return;
        session = std::move(it->second);
        sessions_.erase(it);
    }

    // target_fd shutdown → reader 스레드 recv가 에러/EOF를 받고 종료
    if (session->target_fd >= 0) {
        shutdown(session->target_fd, SHUT_RDWR);
        close(session->target_fd);
        session->target_fd = -1;
    }

    // reader 스레드 종료 대기
    // target_reader가 이미 detach()했으면 joinable()이 false → 건너뜀
    if (session->reader.joinable()) {
        session->reader.join();
    }
}

// ── 스레드 함수 ─────────────────────────────────────────────────────────────

void TunnelAgent::target_reader(uint32_t session_id, int target_fd) {
    constexpr size_t BUF_SIZE = 4096;
    uint8_t buf[BUF_SIZE];

    while (running_) {
        ssize_t n = recv(target_fd, buf, BUF_SIZE, 0);
        if (n <= 0) break;  // EOF 또는 오류

        std::vector<uint8_t> data(buf, buf + n);
        try {
            send_frame(make_data(session_id, std::move(data)));
        } catch (const std::exception&) {
            break;  // server 연결 끊김
        }
    }

    // target EOF → 서버에 CLOSE 통보
    if (running_) {
        try {
            send_frame(make_close(session_id));
        } catch (...) {}
    }

    // 스레드 자체 정리:
    //   close_session()은 join()을 시도하므로 자기 자신은 호출 불가.
    //   대신 reader를 detach()해 스레드를 분리한 뒤 맵에서 직접 제거.
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        auto it = sessions_.find(session_id);
        if (it != sessions_.end()) {
            if (it->second->reader.joinable()) {
                it->second->reader.detach();
            }
            sessions_.erase(it);
        }
    }

    Logger::info("[agent] session " + std::to_string(session_id) +
                 " reader exited");
}

void TunnelAgent::heartbeat_loop() {
    const int64_t timeout_ns =
        static_cast<int64_t>(heartbeat_timeout_s_) * 1'000'000'000LL;

    // 1초 단위로 running_을 확인해 stop() 시 빠르게 종료
    for (int elapsed = 0; running_; ++elapsed) {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        // 타임아웃 감지: 마지막 ACK 이후 경과 시간이 timeout_ns 초과 시 연결 종료
        auto now  = std::chrono::steady_clock::now().time_since_epoch().count();
        auto last = last_ack_ns_.load();
        if (last > 0 && (now - last) > timeout_ns) {
            Logger::warning("[agent] HEARTBEAT_ACK timeout (" +
                            std::to_string(heartbeat_timeout_s_) +
                            "s) — disconnecting for reconnect");
            // stop()을 호출하지 않음: running_=true 유지 → run()이 재연결
            // server_fd_ shutdown → recv_frame 탈출 → cleanup_connection() 호출
            if (server_fd_ >= 0) {
                shutdown(server_fd_, SHUT_RDWR);
            }
            return;  // heartbeat 스레드 종료 → cleanup_connection()이 join
        }

        if (elapsed < heartbeat_interval_s_) continue;
        elapsed = 0;

        try {
            send_frame(make_heartbeat());
            Logger::info("[agent] HEARTBEAT sent");
        } catch (const std::exception& e) {
            Logger::error("[agent] heartbeat send failed: " +
                          std::string(e.what()));
            break;
        }
    }
    Logger::info("[agent] heartbeat thread exited");
}

} // namespace proxy
