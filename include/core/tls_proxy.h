#pragma once

#include <string>
#include <atomic>
#include <cstdint>
#include <unordered_map>
#include <openssl/ssl.h>

/**
 * @file tls_proxy.h
 * @brief Phase 4 — TLS 암호화 TCP 프록시
 *
 * ── EpollProxy vs TlsProxy ────────────────────────────────────────────────────
 *
 *   EpollProxy:  평문 TCP. splice()로 커널 내부 zero-copy 전달.
 *
 *   TlsProxy:    클라이언트 ↔ 프록시 구간을 TLS로 암호화.
 *                구조: 클라이언트(TLS) → [TlsProxy] → 타겟(평문 TCP)
 *
 *                TLS는 암호화/복호화가 필요하므로 유저 공간을 거쳐야 함.
 *                splice() zero-copy는 사용 불가 — SSL_read/SSL_write 사용.
 *
 * ── TLS 핸드셰이크 흐름 ───────────────────────────────────────────────────────
 *
 *   1. 클라이언트가 TCP 연결 수립
 *   2. SSL_accept(): TLS 핸드셰이크 (인증서 교환, 키 협상)
 *      - non-blocking 소켓에서 SSL_ERROR_WANT_READ/WANT_WRITE 반환 가능
 *      - epoll로 재시도 이벤트를 기다리며 핸드셰이크를 완료
 *   3. 핸드셰이크 성공 → SSL_read/SSL_write로 암호화 통신
 *   4. 연결 종료 시 SSL_shutdown() → close()
 *
 * ── SSL_CTX vs SSL ────────────────────────────────────────────────────────────
 *
 *   SSL_CTX: 서버 전체 설정 (인증서, 키, 프로토콜 버전 등).
 *            한 번 생성하고 모든 연결이 공유한다.
 *
 *   SSL:     개별 연결의 TLS 상태. SSL_new(ctx)로 생성.
 *            연결마다 독립적인 핸드셰이크/암호화 상태를 가진다.
 *
 * ── Non-blocking SSL 핸드셰이크 ──────────────────────────────────────────────
 *
 *   SSL_accept()는 단번에 완료되지 않을 수 있다.
 *   non-blocking 소켓에서는 SSL_ERROR_WANT_READ 또는 WANT_WRITE를 반환한다.
 *
 *   이를 처리하기 위해 pending_ssl_ 맵으로 핸드셰이크 진행 중인 연결을 추적:
 *     1. accept() → pending_ssl_[client_fd] = ssl 등록
 *     2. epoll 이벤트마다 SSL_accept() 재시도
 *     3. 성공(ret==1) → target 연결 → connections_ 이동
 *     4. 실패 → SSL_free() + close()
 */
class TlsProxy {
public:
    /**
     * TLS 프록시 초기화
     *
     * Phase 4-A에서 구현:
     *   - OpenSSL 라이브러리 초기화 (OPENSSL_init_ssl)
     *   - SSL_CTX 생성 (TLS_server_method: TLS 1.2/1.3 자동 협상)
     *   - 인증서/키 로드 및 검증
     *   - 리스닝 소켓 생성
     *
     * Phase 4-B에서 추가:
     *   - epoll 인스턴스 생성 + listen_fd 등록
     *
     * @param local_port  클라이언트 연결을 받을 포트
     * @param target_ip   평문 트래픽을 전달할 서버 IP
     * @param target_port 평문 트래픽을 전달할 서버 포트
     * @param cert_file   서버 인증서 경로 (PEM 형식, gen_cert.sh로 생성)
     * @param key_file    서버 개인키 경로 (PEM 형식)
     * @param max_events  epoll_wait이 한 번에 반환할 최대 이벤트 수
     */
    TlsProxy(int local_port, const std::string& target_ip, int target_port,
             const std::string& cert_file, const std::string& key_file,
             int max_events = 64);

    ~TlsProxy();

    /**
     * epoll 이벤트 루프 시작 (Phase 4-B에서 구현)
     *
     * EpollProxy::run()과 구조는 동일하나 accept 후 SSL_accept()로
     * TLS 핸드셰이크를 non-blocking 방식으로 수행한다.
     *
     * 이벤트 분류:
     *   - fd == listen_fd_           → accept_connection()
     *   - fd in pending_ssl_         → continue_handshake()
     *   - fd in connections_         → handle_event()
     */
    void run();

    void stop();

private:
    /**
     * fd 쌍(client ↔ target)의 상태를 저장하는 구조체
     *
     * ssl 필드:
     *   - is_client=true  측 fd: 클라이언트와의 SSL 객체 (non-null)
     *   - is_client=false 측 fd: 타겟 서버는 평문 TCP (null)
     *
     * 이 구조로 handle_event()에서 어느 쪽이 TLS인지 판단한다:
     *   - ssl != null → SSL_read로 읽기, write()로 타겟에 쓰기
     *   - ssl == null → read()로 읽기, SSL_write로 클라이언트에 쓰기
     */
    struct ConnState {
        int  peer_fd;    // 데이터를 전달할 반대쪽 fd
        bool is_client;  // true: 클라이언트 측 fd / false: 타겟 측 fd
        SSL* ssl;        // 클라이언트 측(is_client=true)만 non-null
    };

    // ── SSL 초기화 ────────────────────────────────────────────────────────────

    /**
     * SSL_CTX를 초기화하고 인증서/키를 로드한다. (Phase 4-A에서 구현)
     */
    void init_ssl_context(const std::string& cert_file, const std::string& key_file);

    /** OpenSSL 에러 큐에서 가장 최근 에러 문자열을 반환한다. */
    static std::string ssl_error_string();

    // ── 소켓 초기화 ───────────────────────────────────────────────────────────

    /** fd를 논블로킹 모드로 설정한다. */
    void set_nonblocking(int fd);

    /**
     * 타겟 서버에 블로킹 TCP 연결을 수립한다.
     *
     * connect() 성공 후 set_nonblocking()을 호출해 epoll ET 모드에 맞게 전환.
     */
    int connect_to_target();

    // ── epoll 관리 ────────────────────────────────────────────────────────────

    /** fd를 epoll에 등록한다. */
    void add_to_epoll(int fd, uint32_t events);

    /** 이미 등록된 fd의 관심 이벤트를 변경한다. */
    void mod_epoll(int fd, uint32_t events);

    /** fd를 epoll에서 제거한다. */
    void remove_from_epoll(int fd);

    // ── 이벤트 처리 ───────────────────────────────────────────────────────────

    /**
     * 새 TCP 연결을 수락하고 SSL 핸드셰이크를 시작한다.
     *
     * ET 모드이므로 EAGAIN까지 반복 accept().
     * 각 client_fd에 대해:
     *   1. set_nonblocking(client_fd)
     *   2. SSL_new(ssl_ctx_) + SSL_set_fd(ssl, client_fd)
     *   3. pending_ssl_[client_fd] = ssl 등록
     *   4. add_to_epoll(client_fd, EPOLLIN | EPOLLET)
     *   → 실제 핸드셰이크는 continue_handshake()에서 이벤트 단위로 처리
     */
    void accept_connection();

    /**
     * 진행 중인 SSL_accept() 핸드셰이크를 재시도한다.
     *
     * SSL_accept() 반환값:
     *   - ret == 1                  → 성공. target 연결 후 connections_ 이동.
     *   - SSL_ERROR_WANT_READ       → 다음 EPOLLIN 이벤트에서 재시도.
     *   - SSL_ERROR_WANT_WRITE      → EPOLLOUT 이벤트로 변경 후 재시도.
     *   - 그 외                     → 실패. SSL_free() + close().
     *
     * @param fd  핸드셰이크 중인 client_fd
     */
    void continue_handshake(int fd);

    /**
     * 데이터 읽기/쓰기 이벤트를 처리한다.
     *
     * connections_[fd].is_client 에 따라 방향 결정:
     *   - is_client: forward_client_to_target()
     *   - !is_client: forward_target_to_client()
     *
     * @param fd     이벤트가 발생한 fd
     * @param events epoll이 반환한 이벤트 플래그
     */
    void handle_event(int fd, uint32_t events);

    /**
     * 연결을 완전히 종료하고 자원을 정리한다.
     *
     * 1. remove_from_epoll(fd) + remove_from_epoll(peer_fd)
     * 2. SSL_shutdown(ssl) + SSL_free(ssl)  (클라이언트 측만)
     * 3. close(fd) + close(peer_fd)
     * 4. connections_에서 두 항목 모두 제거
     */
    void close_connection(int fd);

    /**
     * 핸드셰이크 실패/중단 시 pending 연결을 정리한다.
     *
     * 1. remove_from_epoll(fd)
     * 2. SSL_free(ssl) + close(fd)
     * 3. pending_ssl_에서 제거
     */
    void close_pending(int fd);

    /**
     * 클라이언트(TLS)에서 읽어 타겟(평문)으로 전달한다.
     *
     * SSL_read() → write()
     * WANT_READ/WANT_WRITE: epoll이 다음 이벤트를 알려줄 때까지 대기.
     */
    void forward_client_to_target(int client_fd, SSL* ssl, int target_fd);

    /**
     * 타겟(평문)에서 읽어 클라이언트(TLS)로 전달한다.
     *
     * read() → SSL_write()
     * EAGAIN: epoll이 다음 이벤트를 알려줄 때까지 대기.
     */
    void forward_target_to_client(int target_fd, int client_fd, SSL* ssl);

    // ── 멤버 변수 ─────────────────────────────────────────────────────────────

    SSL_CTX*          ssl_ctx_;
    int               listen_fd_;
    int               epoll_fd_;
    std::string       target_ip_;
    int               target_port_;
    int               max_events_;
    std::atomic<bool> running_;

    /**
     * fd → ConnState 매핑 테이블
     *
     * client_fd와 target_fd를 각각 키로 등록:
     *   connections_[client_fd] = { target_fd, is_client=true,  ssl }
     *   connections_[target_fd] = { client_fd, is_client=false, nullptr }
     *
     * handle_event()에서 target_fd 이벤트가 오면
     * connections_[target_fd].peer_fd로 client_fd를 찾고
     * connections_[client_fd].ssl로 SSL 객체를 참조한다.
     */
    std::unordered_map<int, ConnState> connections_;

    /**
     * SSL_accept() 핸드셰이크가 진행 중인 연결 추적 맵
     *
     * pending_ssl_[client_fd] = ssl
     *
     * accept_connection()에서 등록, continue_handshake()에서 처리:
     *   - 성공 → connections_로 이동, pending_ssl_에서 제거
     *   - 실패 → SSL_free + close, pending_ssl_에서 제거
     */
    std::unordered_map<int, SSL*> pending_ssl_;
};
