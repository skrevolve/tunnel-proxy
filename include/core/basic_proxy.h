#pragma once

#include <string>
#include <atomic>
#include <cstdint>

/**
 * @file basic_proxy.h
 * @brief Phase 1 — 멀티스레드 기반 TCP 프록시
 *
 * ┌─────────────┐   connect    ┌──────────────┐   connect    ┌───────────────┐
 * │   클라이언트  │ ──────────▶ │  BasicProxy  │ ──────────▶ │  타겟 서버     │
 * │ (nc, curl 등)│ ◀────────── │  (8080 수신) │ ◀────────── │ (8000 등)     │
 * └─────────────┘  양방향전달  └──────────────┘  양방향전달  └───────────────┘
 *
 * 동작 방식:
 *   1. run()이 accept() 루프를 돌며 클라이언트 연결을 대기한다.
 *   2. 연결이 들어오면 새 스레드를 생성해 handle_connection()을 실행한다.
 *      (연결 1개 = 스레드 2개: client→target, target→client 각 1개)
 *   3. 각 스레드는 forward_data()로 한 방향의 데이터를 복사한다.
 *   4. 어느 쪽이든 연결이 끊기면 반대쪽도 shutdown()으로 종료시킨다.
 *
 * 한계 (Phase 2에서 개선):
 *   - 연결 1개당 스레드 2개 생성 → 동시접속 1000개 = 스레드 2000개
 *   - 스레드 기본 스택 8MB → 1000개 연결 시 메모리 16GB 필요
 *   - 대부분의 시간은 I/O 대기 상태인데도 스레드가 CPU를 점유
 */
class BasicProxy {
public:
    /**
     * 프록시를 초기화하고 리스닝 소켓을 생성한다.
     *
     * 생성자에서 create_listening_socket()을 호출하므로,
     * 포트가 이미 사용 중이거나 권한이 없으면 여기서 예외가 발생한다.
     *
     * @param local_port  클라이언트 연결을 받을 포트 (예: 8080)
     * @param target_ip   트래픽을 전달할 서버 IP (예: "127.0.0.1")
     * @param target_port 트래픽을 전달할 서버 포트 (예: 8000)
     * @throws std::runtime_error 소켓 생성/바인딩 실패 시
     */
    BasicProxy(int local_port, const std::string& target_ip, int target_port);

    /**
     * 소멸자: stop()을 호출하고 리스닝 소켓을 닫는다.
     * RAII 패턴 — BasicProxy 객체가 스코프를 벗어나면 자동으로 정리된다.
     */
    ~BasicProxy();

    /**
     * 프록시 메인 루프를 시작한다. (블로킹 호출)
     *
     * 내부적으로 accept()를 반복 호출하며 클라이언트 연결을 기다린다.
     * 연결이 들어올 때마다 새 스레드를 detach해 handle_connection()을 실행한다.
     * stop()이 호출되거나 시그널을 받을 때까지 반환하지 않는다.
     */
    void run();

    /**
     * 프록시를 중지한다.
     *
     * running_을 false로 설정하고 listen_fd_를 shutdown()해
     * accept()의 블로킹을 강제로 해제한다.
     * 시그널 핸들러(SIGINT, SIGTERM)에서 호출된다.
     */
    void stop();

    /** 프록시 시작 이후 누적 연결 수 (완료된 연결 포함) */
    uint64_t get_total_connections() const;

    /** 현재 데이터를 주고받는 활성 연결 수 */
    uint64_t get_active_connections() const;

private:
    /**
     * 클라이언트 연결을 받을 TCP 소켓을 생성한다.
     *
     * 순서: socket() → setsockopt(SO_REUSEADDR) → bind() → listen()
     *
     * SO_REUSEADDR을 설정하는 이유:
     *   프록시를 재시작할 때 이전 연결의 TIME_WAIT 상태가 남아있으면
     *   같은 포트를 즉시 bind()할 수 없다. SO_REUSEADDR은 이 제한을 해제한다.
     *
     * @param port 바인딩할 포트 번호
     * @return 생성된 소켓 fd
     * @throws std::runtime_error 각 단계 실패 시
     */
    int create_listening_socket(int port);

    /**
     * 타겟 서버에 TCP 연결을 맺는다.
     *
     * 순서: socket() → inet_pton() → connect()
     *
     * inet_pton을 쓰는 이유:
     *   inet_addr()은 오류 시 INADDR_NONE(-1)을 반환하는데,
     *   이 값이 유효한 IP(255.255.255.255)와 구분이 안 된다.
     *   inet_pton은 반환값으로 성공/실패를 명확히 구분한다.
     *
     * @param ip   타겟 서버 IP 문자열 (예: "127.0.0.1")
     * @param port 타겟 서버 포트
     * @return 연결된 소켓 fd
     * @throws std::runtime_error 연결 실패 시
     */
    int connect_to_target(const std::string& ip, int port);

    /**
     * 클라이언트 ↔ 타겟 서버 간 양방향 데이터 전달을 처리한다.
     *
     * 스레드 2개를 생성해 각각 한 방향씩 담당한다:
     *   t1: client_fd → target_fd
     *   t2: target_fd → client_fd
     *
     * 두 스레드가 모두 종료될 때까지 join()으로 대기한 뒤 소켓을 닫는다.
     * (detach가 아닌 join을 쓰는 이유: fd를 너무 일찍 닫으면 안 되기 때문)
     *
     * @param client_fd accept()로 받은 클라이언트 소켓 fd
     */
    void handle_connection(int client_fd);

    /**
     * from_fd에서 읽어 to_fd로 쓰는 단방향 데이터 복사 루프.
     *
     * read()가 0(EOF) 또는 음수(에러)를 반환할 때까지 반복한다.
     * 루프 종료 시 shutdown(to_fd, SHUT_WR)을 호출해
     * 반대 방향 스레드에게 "더 이상 데이터가 없음"을 알린다.
     *
     * write를 루프로 감싸는 이유:
     *   TCP는 스트림 프로토콜이라 write()가 요청한 바이트를 한 번에
     *   전송하지 않을 수 있다(partial write). 전부 전송될 때까지 반복해야 한다.
     *
     * @param from_fd 읽을 소켓 fd
     * @param to_fd   쓸 소켓 fd
     */
    void forward_data(int from_fd, int to_fd);

    // ── 멤버 변수 ─────────────────────────────────────────────────────────────

    int listen_fd_;         // accept()를 기다리는 서버 소켓 fd. 초기값 -1
    std::string target_ip_; // 트래픽을 전달할 대상 서버 IP
    int target_port_;       // 트래픽을 전달할 대상 서버 포트

    /**
     * 실행 상태 플래그. bool 대신 atomic<bool>을 쓰는 이유:
     *   run()은 메인 스레드에서, stop()은 시그널 핸들러(다른 컨텍스트)에서
     *   호출된다. 일반 bool을 여러 스레드에서 동시에 읽고 쓰면 data race.
     *   atomic은 읽기/쓰기를 원자적으로 보장해 race condition을 방지한다.
     */
    std::atomic<bool> running_;

    /**
     * 연결 카운터도 atomic을 쓰는 이유:
     *   handle_connection()은 여러 스레드에서 동시에 실행된다.
     *   일반 uint64_t의 증감은 원자적이지 않아 카운터가 틀릴 수 있다.
     */
    std::atomic<uint64_t> total_connections_;  // 누적 연결 수
    std::atomic<uint64_t> active_connections_; // 현재 데이터 전송 중인 연결 수
};
