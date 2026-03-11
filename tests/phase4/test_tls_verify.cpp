/**
 * @file test_tls_verify.cpp
 * @brief Phase 4-D — TLS 인증서 검증 테스트
 *
 * ── 테스트 구성 ───────────────────────────────────────────────────────────────
 *
 *   1. TlsCtxTest   — SSL_CTX 초기화 수준 테스트 (TlsProxy 생성자)
 *      - 유효 인증서/키 로딩 성공
 *      - 없는 인증서 파일 → runtime_error
 *      - 없는 키 파일 → runtime_error
 *      - 인증서/키 불일치 → runtime_error
 *
 *   2. TlsHandshakeTest — socketpair로 서버/클라이언트를 구성해 실제 핸드셰이크 검증
 *      - 유효 인증서 → 핸드셰이크 성공
 *      - 만료 인증서 + 검증 활성화 클라이언트 → 핸드셰이크 실패
 *      - 평문 TCP 연결 → 핸드셰이크 실패
 *
 * ── make_temp_cert() ─────────────────────────────────────────────────────────
 *
 *   OpenSSL API로 RSA 2048 자체서명 인증서를 메모리에서 직접 생성한다.
 *   openssl 커맨드 의존 없이 어느 환경에서나 동일하게 동작한다.
 *
 *   days_valid > 0: 지금부터 N일 후 만료 (유효 인증서)
 *   days_valid < 0: 과거에 만료됨 (만료 인증서)
 *     예) -1 → not_before: -2일 전, not_after: -1일 전 (어제 만료)
 *
 * ── socketpair 핸드셰이크 테스트 원리 ─────────────────────────────────────────
 *
 *   socketpair(AF_UNIX, SOCK_STREAM): 커널 내부에서 연결된 fd 쌍을 만든다.
 *   sv[0]에 서버 SSL, sv[1]에 클라이언트 SSL을 붙이면
 *   실제 네트워크 없이도 TLS 핸드셰이크를 완전히 재현할 수 있다.
 *
 *   서버(SSL_accept)와 클라이언트(SSL_connect)는 서로 블로킹하므로
 *   별도 스레드에서 실행해야 교착(deadlock)이 발생하지 않는다.
 */

#include <gtest/gtest.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/pem.h>
#include <openssl/evp.h>
#include <openssl/err.h>
#include "core/tls_proxy.h"
#include <sys/socket.h>
#include <unistd.h>
#include <thread>
#include <cstdio>
#include <cstring>
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// 인증서 생성 유틸리티
// ─────────────────────────────────────────────────────────────────────────────

/**
 * 테스트용 임시 인증서/키 파일을 관리하는 RAII 구조체.
 * 소멸 시 자동으로 임시 파일을 삭제한다.
 */
struct TempCert {
    std::string cert_path;
    std::string key_path;

    TempCert() = default;
    TempCert(const TempCert&) = delete;
    TempCert(TempCert&& o) noexcept
        : cert_path(std::move(o.cert_path))
        , key_path(std::move(o.key_path))
    { o.cert_path.clear(); o.key_path.clear(); }

    ~TempCert() {
        if (!cert_path.empty()) std::remove(cert_path.c_str());
        if (!key_path.empty())  std::remove(key_path.c_str());
    }
};

/**
 * RSA 2048 자체서명 인증서를 생성해 /tmp에 저장한다.
 *
 * @param days_valid  양수: 지금부터 N일 후 만료 (유효).
 *                    음수: 이미 만료됨. -1이면 어제 만료.
 * @param id          파일명 충돌 방지용 식별자
 */
static TempCert make_temp_cert(int days_valid, const std::string& id = "default") {
    // RSA 2048 키 생성
    EVP_PKEY_CTX* kctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    EVP_PKEY_keygen_init(kctx);
    EVP_PKEY_CTX_set_rsa_keygen_bits(kctx, 2048);
    EVP_PKEY* pkey = nullptr;
    EVP_PKEY_keygen(kctx, &pkey);
    EVP_PKEY_CTX_free(kctx);

    X509* x = X509_new();
    ASN1_INTEGER_set(X509_get_serialNumber(x), 1);

    if (days_valid >= 0) {
        // 유효 인증서: 지금부터 days_valid일 후 만료
        X509_gmtime_adj(X509_getm_notBefore(x), 0);
        X509_gmtime_adj(X509_getm_notAfter(x),  (long)days_valid * 86400L);
    } else {
        // 만료 인증서: not_before = (days_valid-1)일 전, not_after = days_valid일 전
        // 예) days_valid=-1 → not_before: -2일, not_after: -1일 (어제 만료)
        X509_gmtime_adj(X509_getm_notBefore(x), (long)(days_valid - 1) * 86400L);
        X509_gmtime_adj(X509_getm_notAfter(x),  (long) days_valid      * 86400L);
    }

    X509_set_pubkey(x, pkey);

    X509_NAME* name = X509_get_subject_name(x);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>("localhost"),
                               -1, -1, 0);
    X509_set_issuer_name(x, name);
    X509_sign(x, pkey, EVP_sha256());

    // pid + id 조합으로 병렬 테스트 파일명 충돌 방지
    std::string prefix = "/tmp/proxy_test_" + std::to_string(getpid()) + "_" + id;
    TempCert tc;
    tc.cert_path = prefix + ".crt";
    tc.key_path  = prefix + ".key";

    FILE* f = fopen(tc.cert_path.c_str(), "w");
    PEM_write_X509(f, x);
    fclose(f);

    f = fopen(tc.key_path.c_str(), "w");
    PEM_write_PrivateKey(f, pkey, nullptr, nullptr, 0, nullptr, nullptr);
    fclose(f);

    X509_free(x);
    EVP_PKEY_free(pkey);
    return tc;
}

// ─────────────────────────────────────────────────────────────────────────────
// 1. TlsCtxTest — SSL_CTX 초기화 (TlsProxy 생성자)
// ─────────────────────────────────────────────────────────────────────────────

// 유효한 인증서/키 → TlsProxy 생성 성공
TEST(TlsCtxTest, ValidCertLoads) {
    auto tc = make_temp_cert(365, "valid");
    EXPECT_NO_THROW(
        TlsProxy proxy(19443, "127.0.0.1", 19080, tc.cert_path, tc.key_path)
    );
}

// 존재하지 않는 인증서 파일 → runtime_error
TEST(TlsCtxTest, MissingCertFileThrows) {
    auto tc = make_temp_cert(365, "missingcert");
    EXPECT_THROW(
        TlsProxy(19444, "127.0.0.1", 19080, "/nonexistent/cert.crt", tc.key_path),
        std::runtime_error
    );
}

// 존재하지 않는 키 파일 → runtime_error
TEST(TlsCtxTest, MissingKeyFileThrows) {
    auto tc = make_temp_cert(365, "missingkey");
    EXPECT_THROW(
        TlsProxy(19445, "127.0.0.1", 19080, tc.cert_path, "/nonexistent/key.key"),
        std::runtime_error
    );
}

// 인증서와 키가 서로 다른 쌍 → runtime_error
// SSL_CTX_check_private_key()가 불일치를 감지한다.
TEST(TlsCtxTest, MismatchedKeyThrows) {
    auto tc1 = make_temp_cert(365, "mismatch1");
    auto tc2 = make_temp_cert(365, "mismatch2");
    // tc1의 인증서 + tc2의 키 → 쌍이 맞지 않음
    EXPECT_THROW(
        TlsProxy(19446, "127.0.0.1", 19080, tc1.cert_path, tc2.key_path),
        std::runtime_error
    );
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. TlsHandshakeTest — socketpair 기반 핸드셰이크 검증
// ─────────────────────────────────────────────────────────────────────────────

// 유효한 자체서명 인증서로 핸드셰이크가 성공해야 한다.
// 클라이언트는 SSL_VERIFY_NONE (자체서명이므로 CA 검증 생략).
TEST(TlsHandshakeTest, ValidCertSucceeds) {
    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS | OPENSSL_INIT_LOAD_CRYPTO_STRINGS, nullptr);

    auto tc = make_temp_cert(365, "hs_valid");

    SSL_CTX* server_ctx = SSL_CTX_new(TLS_server_method());
    ASSERT_NE(server_ctx, nullptr);
    ASSERT_EQ(SSL_CTX_use_certificate_file(server_ctx, tc.cert_path.c_str(), SSL_FILETYPE_PEM), 1);
    ASSERT_EQ(SSL_CTX_use_PrivateKey_file(server_ctx, tc.key_path.c_str(), SSL_FILETYPE_PEM), 1);

    // 클라이언트: 자체서명 인증서이므로 CA 체인 검증 생략
    SSL_CTX* client_ctx = SSL_CTX_new(TLS_client_method());
    ASSERT_NE(client_ctx, nullptr);
    SSL_CTX_set_verify(client_ctx, SSL_VERIFY_NONE, nullptr);

    int sv[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

    SSL* server_ssl = SSL_new(server_ctx); SSL_set_fd(server_ssl, sv[0]);
    SSL* client_ssl = SSL_new(client_ctx); SSL_set_fd(client_ssl, sv[1]);

    bool server_ok = false;
    std::thread server_thr([&] { server_ok = (SSL_accept(server_ssl) == 1); });
    bool client_ok = (SSL_connect(client_ssl) == 1);
    server_thr.join();

    EXPECT_TRUE(server_ok) << "서버 SSL_accept 실패";
    EXPECT_TRUE(client_ok) << "클라이언트 SSL_connect 실패";

    SSL_shutdown(client_ssl); SSL_shutdown(server_ssl);
    SSL_free(client_ssl);     SSL_free(server_ssl);
    SSL_CTX_free(client_ctx); SSL_CTX_free(server_ctx);
    close(sv[0]); close(sv[1]);
}

// 만료된 인증서를 가진 서버에 검증 활성화 클라이언트가 접속하면
// 핸드셰이크가 실패해야 한다.
//
// 클라이언트 설정:
//   SSL_VERIFY_PEER: 서버 인증서 검증 활성화
//   load_verify_locations: 자체서명 CA를 신뢰 목록에 추가
//   → CA 자체는 신뢰하지만 만료 여부(X509_V_ERR_CERT_HAS_EXPIRED)는 검증함
TEST(TlsHandshakeTest, ExpiredCertRejected) {
    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS | OPENSSL_INIT_LOAD_CRYPTO_STRINGS, nullptr);

    auto tc = make_temp_cert(-1, "hs_expired");  // 어제 만료

    SSL_CTX* server_ctx = SSL_CTX_new(TLS_server_method());
    ASSERT_NE(server_ctx, nullptr);
    // 만료 인증서도 SSL_CTX에 로딩 자체는 성공한다.
    // 거부는 클라이언트의 핸드셰이크 검증 단계에서 발생한다.
    ASSERT_EQ(SSL_CTX_use_certificate_file(server_ctx, tc.cert_path.c_str(), SSL_FILETYPE_PEM), 1);
    ASSERT_EQ(SSL_CTX_use_PrivateKey_file(server_ctx, tc.key_path.c_str(), SSL_FILETYPE_PEM), 1);

    SSL_CTX* client_ctx = SSL_CTX_new(TLS_client_method());
    ASSERT_NE(client_ctx, nullptr);
    SSL_CTX_set_verify(client_ctx, SSL_VERIFY_PEER, nullptr);
    // 자체서명 인증서를 신뢰 CA로 등록 (CA 불신으로 실패하는 게 아니라 만료로 실패해야 함)
    SSL_CTX_load_verify_locations(client_ctx, tc.cert_path.c_str(), nullptr);

    int sv[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

    SSL* server_ssl = SSL_new(server_ctx); SSL_set_fd(server_ssl, sv[0]);
    SSL* client_ssl = SSL_new(client_ctx); SSL_set_fd(client_ssl, sv[1]);

    // 서버는 accept 시도 — 클라이언트가 alert를 보내면 실패로 반환됨
    std::thread server_thr([&] { SSL_accept(server_ssl); });
    bool client_ok = (SSL_connect(client_ssl) == 1);
    server_thr.join();

    EXPECT_FALSE(client_ok) << "만료 인증서가 수락됨 (거부 예상)";

    SSL_free(client_ssl); SSL_free(server_ssl);
    SSL_CTX_free(client_ctx); SSL_CTX_free(server_ctx);
    close(sv[0]); close(sv[1]);
}

// 평문 TCP 데이터를 보내는 클라이언트는 TLS 서버의 핸드셰이크를 통과할 수 없다.
// 서버의 SSL_accept()가 올바른 ClientHello를 받지 못해 실패해야 한다.
TEST(TlsHandshakeTest, PlainTextRejected) {
    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS | OPENSSL_INIT_LOAD_CRYPTO_STRINGS, nullptr);

    auto tc = make_temp_cert(365, "hs_plain");

    SSL_CTX* server_ctx = SSL_CTX_new(TLS_server_method());
    ASSERT_NE(server_ctx, nullptr);
    ASSERT_EQ(SSL_CTX_use_certificate_file(server_ctx, tc.cert_path.c_str(), SSL_FILETYPE_PEM), 1);
    ASSERT_EQ(SSL_CTX_use_PrivateKey_file(server_ctx, tc.key_path.c_str(), SSL_FILETYPE_PEM), 1);

    int sv[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

    SSL* server_ssl = SSL_new(server_ctx); SSL_set_fd(server_ssl, sv[0]);

    bool server_ok = false;
    std::thread server_thr([&] { server_ok = (SSL_accept(server_ssl) == 1); });

    // 클라이언트는 TLS 없이 평문 HTTP 요청을 전송
    const char* garbage = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    write(sv[1], garbage, strlen(garbage));
    close(sv[1]);  // 연결 종료로 서버가 핸드셰이크 실패를 감지하게 함

    server_thr.join();

    EXPECT_FALSE(server_ok) << "평문 연결이 TLS 서버에 수락됨 (거부 예상)";

    SSL_free(server_ssl);
    SSL_CTX_free(server_ctx);
    close(sv[0]);
}
