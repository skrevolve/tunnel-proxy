#pragma once

#include <string>
#include <atomic>
#include <cstdint>
#include <unordered_map>
#include <sys/epoll.h>

/**
 * epoll 기반 비동기 TCP 프록시 (Phase 2)
 *
 * BasicProxy 대비 개선점:
 * - 연결당 스레드 없이 단일 이벤트 루프로 다수 연결 처리
 * - EPOLLET(edge-triggered) + 논블로킹 소켓
 * - 연결 수 증가에도 O(1) I/O 이벤트 감지
 */
class EpollProxy {
public:
    /**
     * @param local_port  리스닝 포트
     * @param target_ip   타겟 서버 IP
     * @param target_port 타겟 서버 포트
     * @param max_events  epoll_wait 한 번에 처리할 최대 이벤트 수
     */
    EpollProxy(int local_port, const std::string& target_ip, int target_port,
               int max_events = 64);
    ~EpollProxy();

    void run();
    void stop();

    uint64_t get_total_connections() const;
    uint64_t get_active_connections() const;

private:
    /**
     * fd 쌍(client ↔ target)의 상태를 저장하는 구조체
     */
    struct ConnState {
        int peer_fd;     // 반대쪽 fd (client이면 target_fd, 반대도 동일)
        bool is_client;  // true: client 측 fd, false: target 측 fd
    };

    // 소켓 초기화
    int  create_listening_socket(int port);
    void set_nonblocking(int fd);

    // epoll 관리
    void add_to_epoll(int fd, uint32_t events);
    void mod_epoll(int fd, uint32_t events);
    void remove_from_epoll(int fd);

    // 이벤트 처리
    void accept_connection();
    void handle_event(int fd, uint32_t events);
    void close_connection(int fd);

    // 데이터 포워딩 (논블로킹, edge-triggered용)
    void forward_data(int from_fd, int to_fd);

    // 멤버 변수
    int listen_fd_;
    int epoll_fd_;
    std::string target_ip_;
    int target_port_;
    int max_events_;

    std::atomic<bool>     running_;
    std::atomic<uint64_t> total_connections_;
    std::atomic<uint64_t> active_connections_;

    // fd → 연결 상태 맵
    std::unordered_map<int, ConnState> connections_;
};
