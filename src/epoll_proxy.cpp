#include "core/epoll_proxy.h"
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
    // TODO (Phase 2-B): epoll_create1, create_listening_socket, add_to_epoll
    (void)local_port;
}

EpollProxy::~EpollProxy() {
    stop();
    if (epoll_fd_ >= 0) { close(epoll_fd_); epoll_fd_ = -1; }
    if (listen_fd_ >= 0) { close(listen_fd_); listen_fd_ = -1; }
}

void EpollProxy::run() {
    // TODO (Phase 2-C): epoll_wait 이벤트 루프
    running_ = true;
    Logger::info("[EpollProxy] run() — stub, not yet implemented");
    running_ = false;
}

void EpollProxy::stop() {
    running_ = false;
    if (listen_fd_ >= 0) shutdown(listen_fd_, SHUT_RDWR);
}

uint64_t EpollProxy::get_total_connections() const  { return total_connections_.load(); }
uint64_t EpollProxy::get_active_connections() const { return active_connections_.load(); }

int EpollProxy::create_listening_socket(int port) {
    // TODO (Phase 2-B)
    (void)port;
    return -1;
}

void EpollProxy::set_nonblocking(int fd) {
    // TODO (Phase 2-B)
    (void)fd;
}

void EpollProxy::add_to_epoll(int fd, uint32_t events) {
    // TODO (Phase 2-B)
    (void)fd; (void)events;
}

void EpollProxy::mod_epoll(int fd, uint32_t events) {
    // TODO (Phase 2-B)
    (void)fd; (void)events;
}

void EpollProxy::remove_from_epoll(int fd) {
    // TODO (Phase 2-B)
    (void)fd;
}

void EpollProxy::accept_connection() {
    // TODO (Phase 2-C)
}

void EpollProxy::handle_event(int fd, uint32_t events) {
    // TODO (Phase 2-C)
    (void)fd; (void)events;
}

void EpollProxy::close_connection(int fd) {
    // TODO (Phase 2-C)
    (void)fd;
}

void EpollProxy::forward_data(int from_fd, int to_fd) {
    // TODO (Phase 2-C): 논블로킹 read/write + EAGAIN 처리
    (void)from_fd; (void)to_fd;
}
