#include "core/mtls_context.h"

#include <gtest/gtest.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/pem.h>
#include <openssl/evp.h>
#include <openssl/err.h>

#include <sys/socket.h>
#include <unistd.h>
#include <cstdio>
#include <string>
#include <thread>

using namespace proxy;

// ─────────────────────────────────────────────────────────────────────────────
// 인증서 생성 유틸리티
// ─────────────────────────────────────────────────────────────────────────────

struct TempCert {
    std::string cert_path;
    std::string key_path;
    ~TempCert() {
        if (!cert_path.empty()) std::remove(cert_path.c_str());
        if (!key_path.empty())  std::remove(key_path.c_str());
    }
};

struct MtlsCertSet {
    TempCert ca;
    TempCert server;
    TempCert client;
};

static EVP_PKEY* make_rsa_key() {
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    EVP_PKEY_keygen_init(ctx);
    EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 2048);
    EVP_PKEY* pkey = nullptr;
    EVP_PKEY_keygen(ctx, &pkey);
    EVP_PKEY_CTX_free(ctx);
    return pkey;
}

static void save_cert_key(X509* cert, EVP_PKEY* key,
                          const std::string& cert_path,
                          const std::string& key_path) {
    FILE* f = fopen(cert_path.c_str(), "w");
    PEM_write_X509(f, cert);
    fclose(f);
    f = fopen(key_path.c_str(), "w");
    PEM_write_PrivateKey(f, key, nullptr, nullptr, 0, nullptr, nullptr);
    fclose(f);
}

/**
 * CA 서명 인증서 세트 생성 (CA → 서버 → 클라이언트)
 *
 * mTLS는 상호 인증이므로 동일한 CA가 서버/클라이언트 인증서를 모두 서명해야 한다.
 * 자체 서명 인증서(Phase 4)와 달리, 여기서는 CA가 별도로 존재한다.
 *
 * @param id         파일명 충돌 방지용 식별자
 * @param cn         클라이언트 인증서 CN (get_peer_common_name 테스트용)
 * @param days_valid 서버/클라이언트 인증서 유효일 (음수이면 이미 만료)
 */
static MtlsCertSet make_mtls_cert_set(const std::string& id,
                                      const std::string& cn = "test-agent",
                                      int days_valid = 365) {
    std::string prefix = "/tmp/mtls_test_" + std::to_string(getpid()) + "_" + id;

    // ── CA 생성 (자체 서명) ──────────────────────────────────────────────────
    EVP_PKEY* ca_key = make_rsa_key();
    X509* ca_cert = X509_new();
    ASN1_INTEGER_set(X509_get_serialNumber(ca_cert), 1);
    X509_gmtime_adj(X509_getm_notBefore(ca_cert), 0);
    X509_gmtime_adj(X509_getm_notAfter(ca_cert),  365 * 86400L);
    X509_set_pubkey(ca_cert, ca_key);
    X509_NAME* ca_name = X509_get_subject_name(ca_cert);
    X509_NAME_add_entry_by_txt(ca_name, "CN", MBSTRING_ASC,
        reinterpret_cast<const unsigned char*>("TestCA"), -1, -1, 0);
    X509_set_issuer_name(ca_cert, ca_name);
    // CA 기본 제약 추가
    X509V3_CTX v3ctx;
    X509V3_set_ctx_nodb(&v3ctx);
    X509V3_set_ctx(&v3ctx, ca_cert, ca_cert, nullptr, nullptr, 0);
    X509_EXTENSION* ext = X509V3_EXT_conf_nid(
        nullptr, &v3ctx, NID_basic_constraints, "critical,CA:TRUE");
    if (ext) { X509_add_ext(ca_cert, ext, -1); X509_EXTENSION_free(ext); }
    X509_sign(ca_cert, ca_key, EVP_sha256());

    TempCert ca_tc;
    ca_tc.cert_path = prefix + "_ca.crt";
    ca_tc.key_path  = prefix + "_ca.key";
    save_cert_key(ca_cert, ca_key, ca_tc.cert_path, ca_tc.key_path);

    // ── 서버 인증서 (CA 서명) ────────────────────────────────────────────────
    EVP_PKEY* srv_key = make_rsa_key();
    X509* srv_cert = X509_new();
    ASN1_INTEGER_set(X509_get_serialNumber(srv_cert), 2);
    X509_gmtime_adj(X509_getm_notBefore(srv_cert), 0);
    X509_gmtime_adj(X509_getm_notAfter(srv_cert),  days_valid * 86400L);
    X509_set_pubkey(srv_cert, srv_key);
    X509_NAME* srv_name = X509_get_subject_name(srv_cert);
    X509_NAME_add_entry_by_txt(srv_name, "CN", MBSTRING_ASC,
        reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0);
    X509_set_subject_name(srv_cert, srv_name);
    X509_set_issuer_name(srv_cert, ca_name);   // CA가 발급자
    X509_sign(srv_cert, ca_key, EVP_sha256());  // CA 개인키로 서명

    TempCert srv_tc;
    srv_tc.cert_path = prefix + "_srv.crt";
    srv_tc.key_path  = prefix + "_srv.key";
    save_cert_key(srv_cert, srv_key, srv_tc.cert_path, srv_tc.key_path);

    // ── 클라이언트 인증서 (CA 서명) ──────────────────────────────────────────
    EVP_PKEY* cli_key = make_rsa_key();
    X509* cli_cert = X509_new();
    ASN1_INTEGER_set(X509_get_serialNumber(cli_cert), 3);
    X509_gmtime_adj(X509_getm_notBefore(cli_cert), 0);
    X509_gmtime_adj(X509_getm_notAfter(cli_cert),  days_valid * 86400L);
    X509_set_pubkey(cli_cert, cli_key);
    X509_NAME* cli_name = X509_get_subject_name(cli_cert);
    X509_NAME_add_entry_by_txt(cli_name, "CN", MBSTRING_ASC,
        reinterpret_cast<const unsigned char*>(cn.c_str()), -1, -1, 0);
    X509_set_subject_name(cli_cert, cli_name);
    X509_set_issuer_name(cli_cert, ca_name);   // CA가 발급자
    X509_sign(cli_cert, ca_key, EVP_sha256());  // CA 개인키로 서명

    TempCert cli_tc;
    cli_tc.cert_path = prefix + "_cli.crt";
    cli_tc.key_path  = prefix + "_cli.key";
    save_cert_key(cli_cert, cli_key, cli_tc.cert_path, cli_tc.key_path);

    // 정리
    X509_free(ca_cert);  EVP_PKEY_free(ca_key);
    X509_free(srv_cert); EVP_PKEY_free(srv_key);
    X509_free(cli_cert); EVP_PKEY_free(cli_key);

    return MtlsCertSet{std::move(ca_tc), std::move(srv_tc), std::move(cli_tc)};
}

// ─────────────────────────────────────────────────────────────────────────────
// 시나리오 7: create_server / create_client — 유효 인증서로 정상 생성
// ─────────────────────────────────────────────────────────────────────────────
TEST(MtlsContextTest, CreateServerAndClient_ValidCerts_Succeeds) {
    auto certs = make_mtls_cert_set("create");

    EXPECT_NO_THROW(
        MtlsContext::create_server(
            certs.server.cert_path, certs.server.key_path,
            certs.ca.cert_path));

    EXPECT_NO_THROW(
        MtlsContext::create_client(
            certs.client.cert_path, certs.client.key_path,
            certs.ca.cert_path));
}

// ─────────────────────────────────────────────────────────────────────────────
// 시나리오 8: get_peer_common_name — socketpair 핸드셰이크 후 CN 추출
// ─────────────────────────────────────────────────────────────────────────────
TEST(MtlsContextTest, GetPeerCommonName_ReturnsClientCN) {
    const std::string expected_cn = "my-tunnel-agent";
    auto certs = make_mtls_cert_set("cn", expected_cn);

    auto srv_ctx = MtlsContext::create_server(
        certs.server.cert_path, certs.server.key_path, certs.ca.cert_path);
    auto cli_ctx = MtlsContext::create_client(
        certs.client.cert_path, certs.client.key_path, certs.ca.cert_path);

    int sv[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

    std::string server_cn;
    std::thread server_thread([&]() {
        SSL* ssl = SSL_new(srv_ctx.get());
        SSL_set_fd(ssl, sv[0]);
        if (SSL_accept(ssl) == 1) {
            server_cn = MtlsContext::get_peer_common_name(ssl);
        }
        SSL_free(ssl);
        close(sv[0]);
    });

    SSL* cli_ssl = SSL_new(cli_ctx.get());
    SSL_set_fd(cli_ssl, sv[1]);
    SSL_connect(cli_ssl);
    SSL_free(cli_ssl);
    close(sv[1]);

    server_thread.join();
    EXPECT_EQ(server_cn, expected_cn);
}

// ─────────────────────────────────────────────────────────────────────────────
// 시나리오 9: get_cert_expiry — 만료일 문자열 반환
// ─────────────────────────────────────────────────────────────────────────────
TEST(MtlsContextTest, GetCertExpiry_ReturnsNonEmptyString) {
    auto certs = make_mtls_cert_set("expiry");
    std::string expiry = MtlsContext::get_cert_expiry(certs.server.cert_path);
    EXPECT_FALSE(expiry.empty());
}

// ─────────────────────────────────────────────────────────────────────────────
// 시나리오 10: create_server() 잘못된 인증서 경로 → runtime_error
// ─────────────────────────────────────────────────────────────────────────────
TEST(MtlsContextTest, CreateServer_MissingCertFile_Throws) {
    auto certs = make_mtls_cert_set("missing");
    EXPECT_THROW(
        MtlsContext::create_server(
            "/nonexistent/cert.crt",
            certs.server.key_path,
            certs.ca.cert_path),
        std::runtime_error);
}

TEST(MtlsContextTest, CreateServer_MissingKeyFile_Throws) {
    auto certs = make_mtls_cert_set("missingkey");
    EXPECT_THROW(
        MtlsContext::create_server(
            certs.server.cert_path,
            "/nonexistent/server.key",
            certs.ca.cert_path),
        std::runtime_error);
}

// ─────────────────────────────────────────────────────────────────────────────
// 시나리오 11: verify_peer — CA 없는 자체 서명 인증서로 핸드셰이크 실패
// ─────────────────────────────────────────────────────────────────────────────
TEST(MtlsContextTest, VerifyPeer_SelfSignedClientWithoutCA_Fails) {
    // 서버: 정상 CA 세트
    auto server_certs = make_mtls_cert_set("verify_srv");
    // 클라이언트: 다른 CA로 서명된 별도 세트 (server의 CA가 신뢰 안 함)
    auto rogue_certs  = make_mtls_cert_set("verify_rogue");

    auto srv_ctx = MtlsContext::create_server(
        server_certs.server.cert_path,
        server_certs.server.key_path,
        server_certs.ca.cert_path);   // server_certs CA만 신뢰

    // 클라이언트는 rogue CA가 서명한 인증서 제시
    auto cli_ctx = MtlsContext::create_client(
        rogue_certs.client.cert_path,
        rogue_certs.client.key_path,
        server_certs.ca.cert_path);   // 서버 CA는 알지만

    int sv[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

    bool server_verify_result = true;  // 기본값 true, 실패 시 false
    std::thread server_thread([&]() {
        SSL* ssl = SSL_new(srv_ctx.get());
        SSL_set_fd(ssl, sv[0]);
        int ret = SSL_accept(ssl);
        if (ret == 1) {
            std::string err;
            server_verify_result = MtlsContext::verify_peer(ssl, &err);
        } else {
            server_verify_result = false;  // 핸드셰이크 자체 실패
        }
        SSL_free(ssl);
        close(sv[0]);
    });

    SSL* cli_ssl = SSL_new(cli_ctx.get());
    SSL_set_fd(cli_ssl, sv[1]);
    SSL_connect(cli_ssl);
    SSL_free(cli_ssl);
    close(sv[1]);

    server_thread.join();
    EXPECT_FALSE(server_verify_result);
}
