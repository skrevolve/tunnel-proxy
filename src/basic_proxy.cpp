#include "core/basic_proxy.h"
#include "utils/logger.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <thread>
#include <stdexcept>
#include <cerrno>

BasicProxy::BasicProxy(int local_port, const std::string& target_ip, int target_port)
    : listen_fd_(-1)
    , target_ip_(target_ip)
    , target_port_(target_port)
    , running_(false)
    , total_connections_(0)
    , active_connections_(0)
{
    listen_fd_ = create_listening_socket(local_port);
}

BasicProxy::~BasicProxy() {
    stop();
    if (listen_fd_ >= 0) {
        close(listen_fd_);
        listen_fd_ = -1;
    }
}

// Step 4: accept() 루프
void BasicProxy::run() {
    running_ = true;
    Logger::info("Proxy started, listening on fd: " + std::to_string(listen_fd_));

    while (running_) {
        int client_fd = accept(listen_fd_, nullptr, nullptr);
        if (client_fd < 0) {
            if (!running_) break;
            Logger::error("accept failed: " + std::string(strerror(errno)));
            continue;
        }

        std::thread([this, client_fd]() {
            handle_connection(client_fd);
        }).detach();
    }
}

void BasicProxy::stop() {
    running_ = false;
    if (listen_fd_ >= 0) {
        // accept() 블로킹 해제
        shutdown(listen_fd_, SHUT_RDWR);
    }
}

uint64_t BasicProxy::get_total_connections() const {
    return total_connections_.load();
}

uint64_t BasicProxy::get_active_connections() const {
    return active_connections_.load();
}

// Step 3: socket/setsockopt/bind/listen
int BasicProxy::create_listening_socket(int port) {
    Logger::debug("Creating listening socket on port " + std::to_string(port));

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

    return fd;
}

// Step 5: socket/connect
int BasicProxy::connect_to_target(const std::string& ip, int port) {
    Logger::debug("Connecting to " + ip + ":" + std::to_string(port));

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        throw std::runtime_error("socket: " + std::string(strerror(errno)));
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(static_cast<uint16_t>(port));

    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) <= 0) {
        close(fd);
        throw std::runtime_error("inet_pton: invalid IP address: " + ip);
    }

    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        throw std::runtime_error("connect to " + ip + ":" + std::to_string(port) +
                                 " failed: " + strerror(errno));
    }

    return fd;
}

// Step 6: 양방향 스레드 생성
void BasicProxy::handle_connection(int client_fd) {
    total_connections_++;
    active_connections_++;

    Logger::info("New connection (fd=" + std::to_string(client_fd) + ") total=" +
                 std::to_string(total_connections_.load()));

    int target_fd = -1;
    try {
        target_fd = connect_to_target(target_ip_, target_port_);
    } catch (const std::exception& e) {
        Logger::error("Failed to connect to target: " + std::string(e.what()));
        active_connections_--;
        close(client_fd);
        return;
    }

    // client→target / target→client 양방향 포워딩
    std::thread t1([this, client_fd, target_fd]() {
        forward_data(client_fd, target_fd);
    });
    std::thread t2([this, target_fd, client_fd]() {
        forward_data(target_fd, client_fd);
    });

    t1.join();
    t2.join();

    close(client_fd);
    close(target_fd);

    active_connections_--;
    Logger::debug("Connection closed (fd=" + std::to_string(client_fd) + ")");
}

// Step 7: read/write 루프
void BasicProxy::forward_data(int from_fd, int to_fd) {
    constexpr size_t BUF_SIZE = 4096;
    char buffer[BUF_SIZE];

    while (true) {
        ssize_t n = read(from_fd, buffer, BUF_SIZE);
        if (n <= 0) break;  // EOF 또는 에러

        ssize_t written = 0;
        while (written < n) {
            ssize_t w = write(to_fd, buffer + written, static_cast<size_t>(n - written));
            if (w <= 0) return;
            written += w;
        }
    }

    // 한쪽이 닫히면 반대쪽 write도 종료시킴
    shutdown(to_fd, SHUT_WR);
}
