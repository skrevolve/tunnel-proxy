#pragma once

#include <string>
#include <openssl/ssl.h>
#include <openssl/x509.h>

namespace proxy {

/**
 * @file mtls_context.h
 * @brief Phase 7-A — mTLS (mutual TLS) SSL_CTX 래퍼
 *
 * ── TLS vs mTLS ───────────────────────────────────────────────────────────────
 *
 *   일반 TLS:  서버만 인증서를 제시 → 클라이언트가 서버를 신뢰 확인
 *              누구나 서버에 연결 가능 (신원 미확인)
 *
 *   mTLS:     서버와 클라이언트 모두 인증서 제시 → 양방향 신원 확인
 *              Zero Trust 모델: 네트워크 위치와 무관하게
 *              "올바른 인증서를 가진 주체"만 연결 허용
 *
 *   엔진 맥락:
 *     TunnelAgent가 TunnelServer에 연결할 때 서버는 에이전트의 인증서를 검증.
 *     등록된 CA가 서명한 인증서 없이는 연결을 거부 → 무단 에이전트 차단.
 *
 * ── CA 체인 검증 원리 ─────────────────────────────────────────────────────────
 *
 *   CA (Certificate Authority): 인증서에 서명하는 신뢰 앵커.
 *   피어 인증서의 서명이 CA 인증서의 공개키로 검증되면 → 신뢰.
 *   서명이 맞지 않거나 CA가 로딩되지 않았으면 → 핸드셰이크 실패.
 *
 *   SSL_CTX_load_verify_locations(): CA 인증서를 컨텍스트에 로딩.
 *   SSL_CTX_set_verify(SSL_VERIFY_PEER):
 *     - 서버 측: 클라이언트 인증서 요청
 *     - | SSL_VERIFY_FAIL_IF_NO_PEER_CERT: 인증서 미제출 시 즉시 거부
 *   SSL_get_verify_result(): 핸드셰이크 후 검증 결과 (X509_V_OK이어야 통과)
 *
 * ── 인증서 검증 체크리스트 ────────────────────────────────────────────────────
 *
 *   1. 서명 유효성  — CA의 공개키로 서명 검증 (OpenSSL이 자동 수행)
 *   2. 유효 기간    — NotBefore ≤ 현재 시각 ≤ NotAfter
 *   3. 폐기 여부    — CRL/OCSP (Phase 7 범위 외, 운영 환경에서 추가 필요)
 *   4. CN/SAN 확인  — 연결 대상 호스트와 인증서 주체 일치 (verify_peer_hostname)
 *
 * ── 사용 예시 ─────────────────────────────────────────────────────────────────
 *
 *   // 서버 측 (클라이언트 인증서 요구)
 *   auto ctx = MtlsContext::create_server("server.crt", "server.key", "ca.crt");
 *   SSL* ssl = SSL_new(ctx.get());
 *   SSL_set_fd(ssl, client_fd);
 *   SSL_accept(ssl);
 *   if (!MtlsContext::verify_peer(ssl)) { SSL_free(ssl); ... }
 *
 *   // 클라이언트 측 (서버에 자신의 인증서 제시)
 *   auto ctx = MtlsContext::create_client("client.crt", "client.key", "ca.crt");
 *   SSL* ssl = SSL_new(ctx.get());
 *   SSL_set_fd(ssl, server_fd);
 *   SSL_connect(ssl);
 *   if (!MtlsContext::verify_peer(ssl)) { SSL_free(ssl); ... }
 */
class MtlsContext {
public:
    ~MtlsContext();

    // 이동만 허용 (SSL_CTX는 복사 불가)
    MtlsContext(MtlsContext&& other) noexcept;
    MtlsContext& operator=(MtlsContext&& other) noexcept;
    MtlsContext(const MtlsContext&)            = delete;
    MtlsContext& operator=(const MtlsContext&) = delete;

    // ── 팩토리 함수 ─────────────────────────────────────────────────────────

    /**
     * 서버용 mTLS 컨텍스트 생성
     *
     * 설정 내용:
     *   - cert_file/key_file: 서버 인증서 + 개인키 로딩
     *   - ca_file: 클라이언트 인증서 서명 CA 로딩
     *   - SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT:
     *       클라이언트 인증서 미제출 시 핸드셰이크 거부
     *
     * @param cert_file 서버 인증서 경로 (PEM)
     * @param key_file  서버 개인키 경로 (PEM)
     * @param ca_file   클라이언트 인증서를 서명한 CA 인증서 경로 (PEM)
     * @throws std::runtime_error 파일 로딩 실패 또는 OpenSSL 에러
     */
    static MtlsContext create_server(const std::string& cert_file,
                                     const std::string& key_file,
                                     const std::string& ca_file);

    /**
     * 클라이언트용 mTLS 컨텍스트 생성
     *
     * 설정 내용:
     *   - cert_file/key_file: 클라이언트 인증서 + 개인키 로딩 (서버에 제시)
     *   - ca_file: 서버 인증서를 서명한 CA 로딩 (서버 인증서 검증용)
     *   - SSL_VERIFY_PEER: 서버 인증서 검증 활성화
     *
     * @param cert_file 클라이언트 인증서 경로 (PEM)
     * @param key_file  클라이언트 개인키 경로 (PEM)
     * @param ca_file   서버 인증서를 서명한 CA 인증서 경로 (PEM)
     * @throws std::runtime_error 파일 로딩 실패 또는 OpenSSL 에러
     */
    static MtlsContext create_client(const std::string& cert_file,
                                     const std::string& key_file,
                                     const std::string& ca_file);

    // ── 컨텍스트 접근 ────────────────────────────────────────────────────────

    /** 내부 SSL_CTX 포인터 반환 (SSL_new 등에 전달용) */
    SSL_CTX* get() const { return ctx_; }

    // ── 핸드셰이크 후 검증 ───────────────────────────────────────────────────

    /**
     * 핸드셰이크 완료 후 피어 인증서 추가 검증
     *
     * OpenSSL이 핸드셰이크 시 자동으로 서명·CA 체인을 검증하지만,
     * 이 함수는 그 결과를 명시적으로 확인하고 추가 항목을 검사한다:
     *
     *   1. SSL_get_verify_result() == X509_V_OK
     *      — CA 서명 검증 + 유효 기간이 모두 통과했는지 확인
     *   2. SSL_get_peer_certificate() != nullptr
     *      — 피어가 실제로 인증서를 제출했는지 확인
     *      (SSL_VERIFY_FAIL_IF_NO_PEER_CERT로도 차단되지만 이중 확인)
     *   3. 인증서 유효 기간 (X509_cmp_current_time)
     *      — NotBefore ≤ now ≤ NotAfter
     *
     * @param ssl       핸드셰이크가 완료된 SSL 객체
     * @param error_out 실패 원인 문자열 (nullptr이면 무시)
     * @return true = 검증 통과, false = 실패
     */
    static bool verify_peer(SSL* ssl, std::string* error_out = nullptr);

    /**
     * 피어 인증서의 Subject CN (Common Name) 추출
     *
     * mTLS에서 CN은 에이전트 ID나 서비스 이름으로 사용된다.
     * Phase 7-C의 접근 정책에서 "어떤 CN을 가진 클라이언트가 어느 터널에 접근 가능한가"를
     * 결정할 때 이 함수로 추출한 CN을 사용한다.
     *
     * @param ssl 핸드셰이크가 완료된 SSL 객체
     * @return CN 문자열 (인증서 없거나 파싱 실패 시 빈 문자열)
     */
    static std::string get_peer_common_name(SSL* ssl);

    /**
     * 인증서 파일에서 만료 일시 문자열 반환 (로깅용)
     *
     * @param cert_file PEM 인증서 경로
     * @return "YYYY-MM-DD HH:MM:SS UTC" 형식 문자열 또는 에러 메시지
     */
    static std::string get_cert_expiry(const std::string& cert_file);

private:
    explicit MtlsContext(SSL_CTX* ctx) : ctx_(ctx) {}

    /// 공통 SSL_CTX 초기화 (인증서 + 개인키 로딩 + CA 검증 위치 설정)
    static SSL_CTX* create_base_ctx(const std::string& cert_file,
                                    const std::string& key_file,
                                    const std::string& ca_file);

    SSL_CTX* ctx_{nullptr};
};

} // namespace proxy
