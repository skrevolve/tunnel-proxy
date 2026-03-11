#pragma once

#include <string>
#include <atomic>
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
     * @param local_port  클라이언트 연결을 받을 포트
     * @param target_ip   평문 트래픽을 전달할 서버 IP
     * @param target_port 평문 트래픽을 전달할 서버 포트
     * @param cert_file   서버 인증서 경로 (PEM 형식, gen_cert.sh로 생성)
     * @param key_file    서버 개인키 경로 (PEM 형식)
     */
    TlsProxy(int local_port, const std::string& target_ip, int target_port,
             const std::string& cert_file, const std::string& key_file);

    ~TlsProxy();

    /**
     * epoll 이벤트 루프 시작 (Phase 4-B에서 구현)
     *
     * EpollProxy::run()과 구조는 동일하나
     * accept 후 SSL_accept()로 TLS 핸드셰이크를 추가로 수행한다.
     */
    void run();

    void stop();

private:
    /**
     * SSL_CTX를 초기화하고 인증서/키를 로드한다.
     *
     * 실패 시 OpenSSL 에러 스택을 읽어 예외 메시지에 포함.
     */
    void init_ssl_context(const std::string& cert_file, const std::string& key_file);

    /** OpenSSL 에러 큐에서 가장 최근 에러 문자열을 반환한다. */
    static std::string ssl_error_string();

    SSL_CTX*          ssl_ctx_;
    int               listen_fd_;
    std::string       target_ip_;
    int               target_port_;
    std::atomic<bool> running_;
};
