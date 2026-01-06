#include "core/basic_proxy.h"
#include "utils/logger.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <thread>
#include <stdexcept>

BasicProxy::BasicProxy(int local_port, const std::string& target_ip, int target_port)
    : listen_fd_(-1)
    , target_ip_(target_ip)
    , target_port_(target_port)
    , running_(false)
    , total_connections_(0)
    , active_connections_(0)
{
    listen_fd_ = create_listening_socket(local_port);
    if (listen_fd_ < 0) {
        throw std::runtime_error("Failed to create listening socket");
    }
}

BasicProxy::~BasicProxy() {
    stop();
    if (listen_fd_ >= 0) {
        close(listen_fd_);
    }
}

void BasicProxy::run() {
    running_ = true;
    Logger::info("Proxy started, listening on fd: " + std::to_string(listen_fd_));
    
    while (running_) {
        // TODO: accept() 구현
        // TODO: 새 스레드에서 handle_connection() 호출
        
        // 힌트:
        // int client_fd = accept(listen_fd_, nullptr, nullptr);
        // std::thread([this, client_fd]() {
        //     handle_connection(client_fd);
        // }).detach();
    }
}

void BasicProxy::stop() {
    running_ = false;
}

uint64_t BasicProxy::get_total_connections() const {
    return total_connections_.load();
}

uint64_t BasicProxy::get_active_connections() const {
    return active_connections_.load();
}

int BasicProxy::create_listening_socket(int port) {
    // TODO: 소켓 생성 및 바인딩 구현
    
    // 힌트:
    // 1. socket() 호출
    // 2. setsockopt(SO_REUSEADDR)
    // 3. bind()
    // 4. listen()
    
    Logger::debug("Creating listening socket on port " + std::to_string(port));
    
    // 임시 구현 (컴파일 에러 방지)
    (void)port;
    return -1;
}

int BasicProxy::connect_to_target(const std::string& ip, int port) {
    // TODO: 타겟 서버 연결 구현
    
    // 힌트:
    // 1. socket() 호출
    // 2. sockaddr_in 설정
    // 3. connect()
    
    Logger::debug("Connecting to " + ip + ":" + std::to_string(port));
    
    // 임시 구현 (컴파일 에러 방지)
    (void)ip;
    (void)port;
    return -1;
}

void BasicProxy::handle_connection(int client_fd) {
    // TODO: 연결 처리 구현
    
    total_connections_++;
    active_connections_++;
    
    Logger::info("New connection from client fd: " + std::to_string(client_fd));
    
    // 힌트:
    // 1. connect_to_target() 호출하여 타겟 서버 연결
    // 2. 두 개의 스레드 생성:
    //    - client → target (forward_data)
    //    - target → client (forward_data)
    // 3. 스레드 종료 대기
    // 4. 소켓 닫기
    
    active_connections_--;
    close(client_fd);
}

void BasicProxy::forward_data(int from_fd, int to_fd) {
    // TODO: 데이터 전달 구현
    
    // 힌트:
    // char buffer[4096];
    // while (true) {
    //     ssize_t n = read(from_fd, buffer, sizeof(buffer));
    //     if (n <= 0) break;
    //     write(to_fd, buffer, n);
    // }
    
    (void)from_fd;
    (void)to_fd;
}
