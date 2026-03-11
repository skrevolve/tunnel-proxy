#pragma once

#include <string>
#include <atomic>
#include <cstdint>
#include <unordered_map>
#include <netinet/in.h>

/**
 * @file udp_proxy.h
 * @brief Phase 5 — UDP 포워딩 프록시
 *
 * ── TCP 프록시와의 차이 ───────────────────────────────────────────────────────
 *
 *   TCP: connect() 후 read/write. fd 하나 = 연결 하나.
 *
 *   UDP: 연결 개념이 없다. 하나의 소켓으로 여러 클라이언트의 패킷을 수신한다.
 *        recvfrom()이 패킷마다 "어디서 왔는지(sockaddr_in)"를 알려준다.
 *        응답을 보낼 때는 그 주소로 sendto()한다.
 *
 * ── 세션 테이블이 필요한 이유 ────────────────────────────────────────────────
 *
 *   클라이언트(A) → listen_sock → 타겟 → 응답 → ???
 *
 *   응답이 타겟에서 돌아올 때 listen_sock으로는 "어떤 클라이언트에게 돌려줘야
 *   하는지" 알 방법이 없다. 클라이언트마다 전용 target_fd를 만들고,
 *   target_fd → client_addr 역방향 매핑을 세션 테이블에 저장한다.
 *
 *   client_addr("ip:port") ─→ Session { target_fd, client_addr }
 *   target_fd              ─→ client_addr key (역방향 조회)
 *
 * ── 패킷 흐름 ────────────────────────────────────────────────────────────────
 *
 *   클라이언트 → 타겟:
 *     1. recvfrom(listen_fd) → buf + client_addr
 *     2. 세션 테이블에서 client_addr 검색
 *        없으면: target_fd = socket() + connect(target) 새로 생성
 *        있으면: 기존 target_fd 재사용
 *     3. send(target_fd, buf)  (connect 후이므로 sendto 불필요)
 *
 *   타겟 → 클라이언트:
 *     1. recvfrom(target_fd) → buf  (connect했으므로 타겟에서만 수신)
 *     2. sessions_by_target_[target_fd] → client_addr
 *     3. sendto(listen_fd_, buf, client_addr)
 *
 * ── UDP connect() 의미 ───────────────────────────────────────────────────────
 *
 *   UDP 소켓에 connect()를 호출하면 TCP처럼 핸드셰이크를 하지 않는다.
 *   커널에 "이 소켓은 해당 주소와만 통신한다"고 등록하는 것이다.
 *   이후 send()로 타겟에 전송하고, recv()로 타겟에서만 수신할 수 있다.
 *   다른 주소에서 온 패킷은 커널이 자동으로 필터링한다.
 */
class UdpProxy {
public:
    /**
     * UDP 프록시 초기화
     *
     * - UDP 리스닝 소켓 생성 + bind
     * - epoll 인스턴스 생성
     * - listen_fd를 epoll에 등록
     * - target_addr_ 미리 계산 (패킷마다 inet_pton 반복 방지)
     *
     * @param local_port  클라이언트 패킷을 수신할 포트
     * @param target_ip   포워딩할 타겟 서버 IP
     * @param target_port 포워딩할 타겟 서버 포트
     * @param max_events  epoll_wait 최대 이벤트 수
     */
    UdpProxy(int local_port, const std::string& target_ip, int target_port,
             int max_events = 64);

    ~UdpProxy();

    /**
     * epoll 이벤트 루프 시작
     *
     * 이벤트 분류:
     *   - fd == listen_fd_ → handle_client_data()  (클라이언트→타겟)
     *   - 그 외            → handle_target_data(fd) (타겟→클라이언트)
     */
    void run();

    void stop();

private:
    /**
     * 세션 상태
     *
     * 클라이언트 1개당 Session 1개 생성.
     * target_fd: 이 클라이언트 전용으로 타겟과 통신하는 UDP 소켓.
     *            connect()로 특정 타겟에 바인딩되어 있어 send()/recv() 사용.
     * client_addr: 응답을 돌려줄 클라이언트 주소.
     */
    struct Session {
        int         target_fd;
        sockaddr_in client_addr;
    };

    // ── 소켓 유틸리티 ─────────────────────────────────────────────────────────

    void set_nonblocking(int fd);

    // ── epoll 관리 ────────────────────────────────────────────────────────────

    void add_to_epoll(int fd, uint32_t events);
    void remove_from_epoll(int fd);

    // ── 이벤트 처리 ───────────────────────────────────────────────────────────

    /**
     * listen_fd에 수신 이벤트 — 클라이언트 패킷을 타겟으로 포워딩한다.
     *
     * ET 모드이므로 EAGAIN까지 반복 recvfrom().
     * 패킷마다:
     *   1. recvfrom(listen_fd_, buf, ..., &client_addr)
     *   2. get_or_create_session(client_addr)으로 target_fd 확보
     *   3. send(target_fd, buf, n)
     */
    void handle_client_data();

    /**
     * target_fd에 수신 이벤트 — 타겟 응답을 클라이언트로 포워딩한다.
     *
     * ET 모드이므로 EAGAIN까지 반복 recv().
     * 패킷마다:
     *   1. recv(target_fd, buf)
     *   2. sessions_by_target_[target_fd] → client key → client_addr
     *   3. sendto(listen_fd_, buf, ..., client_addr)
     */
    void handle_target_data(int target_fd);

    // ── 세션 관리 ─────────────────────────────────────────────────────────────

    /**
     * 클라이언트 주소에 해당하는 세션을 반환한다.
     *
     * 세션이 없으면 새로 생성한다:
     *   1. socket(SOCK_DGRAM) + connect(target_addr_)
     *   2. set_nonblocking(target_fd)
     *   3. add_to_epoll(target_fd, EPOLLIN | EPOLLET)
     *   4. sessions_by_client_, sessions_by_target_에 등록
     *
     * @return 세션 포인터. 생성 실패 시 nullptr.
     */
    Session* get_or_create_session(const sockaddr_in& client_addr);

    /**
     * target_fd에 대응하는 세션을 닫고 자원을 정리한다.
     *
     * 1. remove_from_epoll(target_fd)
     * 2. close(target_fd)
     * 3. sessions_by_client_, sessions_by_target_에서 제거
     */
    void close_session(int target_fd);

    /**
     * sockaddr_in을 "ip:port" 문자열 키로 변환한다.
     *
     * unordered_map 키로 사용.
     * sockaddr_in을 직접 키로 쓰면 커스텀 해시/비교 함수가 필요하므로
     * 문자열로 변환해 표준 해시를 활용한다.
     */
    static std::string addr_to_key(const sockaddr_in& addr);

    // ── 멤버 변수 ─────────────────────────────────────────────────────────────

    int         listen_fd_;
    int         epoll_fd_;
    std::string target_ip_;
    int         target_port_;
    sockaddr_in target_addr_;   // sendto 시 매번 재계산하지 않도록 미리 저장
    int         max_events_;
    std::atomic<bool> running_;

    /**
     * 클라이언트 주소("ip:port") → 세션
     *
     * 새 클라이언트 패킷이 오면 여기서 세션을 찾는다.
     * 없으면 get_or_create_session()이 새로 생성해 등록한다.
     */
    std::unordered_map<std::string, Session> sessions_by_client_;

    /**
     * target_fd → 클라이언트 주소 키
     *
     * 타겟 응답이 오면 target_fd로 어떤 클라이언트에게 돌려줄지 찾는다.
     * sessions_by_client_[key].client_addr로 sendto한다.
     *
     * TODO Phase 5-B: last_activity 추가해 타임아웃 세션 정리
     */
    std::unordered_map<int, std::string> sessions_by_target_;
};
