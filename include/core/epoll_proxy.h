#pragma once

#include <string>
#include <atomic>
#include <cstdint>
#include <unordered_map>
#include <sys/epoll.h>

/**
 * @file epoll_proxy.h
 * @brief Phase 2 — epoll 기반 비동기 TCP 프록시
 *
 * ── BasicProxy(Phase 1) vs EpollProxy(Phase 2) 비교 ──────────────────────────
 *
 *   BasicProxy:  연결 1개 → 스레드 2개 생성
 *                연결 1000개 → 스레드 2000개, 스택 메모리 ~16GB
 *                대부분의 시간: I/O 대기 (스레드가 CPU를 점유하면서 아무것도 안 함)
 *
 *   EpollProxy:  연결 N개 → 스레드 1개 (이벤트 루프)
 *                커널이 I/O 준비된 fd만 알려줌 → 준비된 것만 처리
 *                연결 수가 늘어도 CPU 사용량이 선형으로 증가하지 않음
 *
 * ── epoll 동작 원리 ───────────────────────────────────────────────────────────
 *
 *   1. epoll_create1()  : 커널에 epoll 인스턴스 생성 (epoll_fd_ 반환)
 *   2. epoll_ctl(ADD)   : 관심있는 fd를 등록 (listen_fd, client_fd 등)
 *   3. epoll_wait()     : I/O 준비된 fd가 생길 때까지 블로킹 대기
 *                         준비되면 이벤트 배열에 담아 반환
 *   4. 이벤트 처리       : listen_fd면 accept, 나머지면 데이터 읽기/쓰기
 *   5. 2~4 반복
 *
 * ── Edge-Triggered(ET) vs Level-Triggered(LT) ────────────────────────────────
 *
 *   LT (기본값): 버퍼에 데이터가 남아있는 한 계속 이벤트 발생
 *                → 읽기를 완료하지 않아도 다음 epoll_wait에서 또 알려줌
 *                → 구현이 쉽지만 불필요한 wake-up 발생
 *
 *   ET (EPOLLET): 상태 변화(새 데이터 도착)가 있을 때만 1번 이벤트 발생
 *                 → 이벤트를 받으면 EAGAIN이 날 때까지 데이터를 전부 소진해야 함
 *                 → 시스템 콜 횟수 감소, 성능 우위
 *                 → Phase 3 zero-copy(splice)의 전제 조건
 *
 * ── EAGAIN ────────────────────────────────────────────────────────────────────
 *
 *   논블로킹 소켓에서 read/write할 데이터가 없을 때 반환되는 에러 코드.
 *   ET 모드에서는 EAGAIN이 오면 "지금 읽을 데이터를 전부 읽었다"는 의미이므로
 *   이벤트 루프로 돌아가 다음 epoll_wait을 기다려야 한다.
 *   EAGAIN을 무시하고 계속 읽으면 무한 루프 또는 잘못된 처리가 된다.
 */
class EpollProxy {
public:
    /**
     * epoll 프록시 초기화
     *
     * Phase 2-B에서 구현:
     *   - epoll_create1(EPOLL_CLOEXEC)으로 epoll 인스턴스 생성
     *   - create_listening_socket()으로 논블로킹 리스닝 소켓 생성
     *   - add_to_epoll()으로 listen_fd를 epoll에 등록
     *
     * @param local_port  클라이언트 연결을 받을 포트
     * @param target_ip   트래픽을 전달할 서버 IP
     * @param target_port 트래픽을 전달할 서버 포트
     * @param max_events  epoll_wait이 한 번에 반환할 최대 이벤트 수
     *                    너무 크면 메모리 낭비, 너무 작으면 이벤트 처리 지연
     *                    일반적으로 64~1024 사용
     */
    EpollProxy(int local_port, const std::string& target_ip, int target_port,
               int max_events = 64);

    /** 소멸자: epoll_fd, listen_fd 순서로 닫는다 */
    ~EpollProxy();

    /**
     * epoll 이벤트 루프 시작 (블로킹 호출)
     *
     * Phase 2-C에서 구현:
     *   while (running_) {
     *     epoll_wait(epoll_fd_, events, max_events_, -1);
     *     for each event:
     *       if fd == listen_fd_  → accept_connection()
     *       else                 → handle_event(fd, events)
     *   }
     */
    void run();

    /** 이벤트 루프를 중지하고 listen_fd를 shutdown한다 */
    void stop();

    uint64_t get_total_connections() const;
    uint64_t get_active_connections() const;

private:
    /**
     * fd 쌍(client ↔ target)의 상태를 저장하는 구조체
     *
     * 왜 peer_fd를 저장하는가:
     *   epoll 이벤트는 "fd X에 이벤트 발생"만 알려준다.
     *   client_fd에 이벤트가 오면 데이터를 target_fd로 보내야 하는데,
     *   이 매핑을 저장해두지 않으면 반대쪽 fd를 찾을 방법이 없다.
     *   또한 한쪽 fd가 닫힐 때 반대쪽도 함께 정리하기 위해 필요하다.
     *
     * connections_ 맵에 client_fd와 target_fd를 각각 키로 등록한다:
     *   connections_[client_fd] = { target_fd, is_client=true  }
     *   connections_[target_fd] = { client_fd, is_client=false }
     */
    struct ConnState {
        int  peer_fd;    // 데이터를 전달할 반대쪽 fd
        bool is_client;  // true: 클라이언트 측 fd / false: 타겟 측 fd
    };

    // ── 소켓 초기화 ───────────────────────────────────────────────────────────

    /**
     * 타겟 서버에 논블로킹 TCP 연결을 수립한다.
     *
     * socket() → connect() 순서로 연결.
     * accept_connection()에서 client_fd마다 한 번씩 호출된다.
     * 연결 실패 시 예외를 던진다.
     */
    int connect_to_target(const std::string& ip, int port);

    /**
     * 논블로킹 리스닝 소켓 생성
     *
     * BasicProxy::create_listening_socket()과 거의 같지만
     * set_nonblocking()을 추가로 호출한다.
     * epoll ET 모드에서는 소켓이 반드시 논블로킹이어야 한다.
     * 블로킹 소켓에서 read()를 호출하면 데이터가 없을 때 무한 대기한다.
     */
    int create_listening_socket(int port);

    /**
     * fd를 논블로킹 모드로 설정한다.
     *
     * fcntl(fd, F_GETFL)로 현재 플래그를 읽고
     * fcntl(fd, F_SETFL, flags | O_NONBLOCK)으로 논블로킹 추가.
     *
     * 논블로킹이면 read/write 시 데이터가 없어도 즉시 반환하고
     * errno에 EAGAIN을 설정한다. 이걸 보고 이벤트 루프로 돌아간다.
     */
    void set_nonblocking(int fd);

    // ── epoll 관리 ────────────────────────────────────────────────────────────

    /**
     * fd를 epoll 인스턴스에 등록한다.
     *
     * epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &event) 호출.
     * events 인자로 보통 EPOLLIN | EPOLLET를 전달한다.
     *   EPOLLIN:  읽을 데이터가 생겼을 때 알림
     *   EPOLLET:  edge-triggered 모드 활성화
     */
    void add_to_epoll(int fd, uint32_t events);

    /**
     * 이미 등록된 fd의 관심 이벤트를 변경한다.
     *
     * epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &event) 호출.
     * 예: 쓰기 버퍼가 가득 찼을 때 EPOLLOUT을 추가해 쓸 수 있을 때 알림.
     * (Phase 2-C에서 활용)
     */
    void mod_epoll(int fd, uint32_t events);

    /**
     * fd를 epoll에서 제거한다.
     *
     * epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr) 호출.
     * 연결이 끊길 때 close_connection()에서 호출된다.
     * 제거하지 않으면 닫힌 fd에 대한 이벤트가 계속 발생한다.
     */
    void remove_from_epoll(int fd);

    // ── 이벤트 처리 ───────────────────────────────────────────────────────────

    /**
     * 새 클라이언트 연결을 수락한다. (Phase 2-C에서 구현)
     *
     * ET 모드에서 listen_fd에 이벤트가 오면 대기 중인 연결이 여러 개일 수 있다.
     * EAGAIN이 올 때까지 accept()를 반복해 모두 처리해야 한다.
     *
     * 각 client_fd에 대해:
     *   1. set_nonblocking(client_fd)
     *   2. connect_to_target()으로 target_fd 생성
     *   3. set_nonblocking(target_fd)
     *   4. connections_에 양방향 매핑 등록
     *   5. 두 fd를 epoll에 등록
     */
    void accept_connection();

    /**
     * 데이터 읽기/쓰기 이벤트를 처리한다. (Phase 2-C에서 구현)
     *
     * connections_[fd].peer_fd를 찾아 forward_data(fd, peer_fd) 호출.
     * EPOLLERR, EPOLLHUP 이벤트면 close_connection() 호출.
     *
     * @param fd     이벤트가 발생한 fd
     * @param events epoll이 반환한 이벤트 플래그
     */
    void handle_event(int fd, uint32_t events);

    /**
     * 연결을 완전히 종료하고 자원을 정리한다. (Phase 2-C에서 구현)
     *
     * 1. remove_from_epoll(fd) + remove_from_epoll(peer_fd)
     * 2. close(fd) + close(peer_fd)
     * 3. connections_에서 두 항목 모두 제거
     * 4. active_connections_ 감소
     */
    void close_connection(int fd);

    /**
     * from_fd에서 읽어 to_fd로 쓰는 논블로킹 데이터 복사. (Phase 2-C에서 구현)
     *
     * ET 모드이므로 EAGAIN이 올 때까지 읽기를 반복해야 한다.
     * BasicProxy::forward_data()와 달리 블로킹하지 않고 즉시 반환한다.
     *
     * EAGAIN 처리 흐름:
     *   read() 반환값 == -1 && errno == EAGAIN → 현재 읽을 데이터 없음 → 반환
     *   read() 반환값 == 0                     → EOF, 연결 종료 → close_connection()
     *   read() 반환값 > 0                      → to_fd에 write 후 계속 읽기
     */
    void forward_data(int from_fd, int to_fd);

    // ── 멤버 변수 ─────────────────────────────────────────────────────────────

    int listen_fd_;    // 클라이언트 연결을 기다리는 서버 소켓 fd
    int epoll_fd_;     // epoll 인스턴스 fd. epoll_create1()이 반환
    std::string target_ip_;
    int target_port_;
    int max_events_;   // epoll_wait에 전달할 이벤트 배열 크기

    std::atomic<bool>     running_;
    std::atomic<uint64_t> total_connections_;
    std::atomic<uint64_t> active_connections_;

    /**
     * fd → ConnState 매핑 테이블
     *
     * unordered_map을 쓰는 이유:
     *   map(레드블랙트리)은 O(log n) 탐색이지만
     *   unordered_map(해시테이블)은 평균 O(1) 탐색.
     *   이벤트 루프는 매 이벤트마다 이 맵을 조회하므로 O(1)이 중요하다.
     *
     * 주의: 단일 스레드 이벤트 루프에서만 접근하므로 mutex 불필요.
     *       멀티스레드로 전환 시 동기화 필요.
     */
    std::unordered_map<int, ConnState> connections_;
};
