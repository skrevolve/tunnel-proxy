#include "core/mtls_context.h"

#include <stdexcept>
#include <string>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/pem.h>
#include <openssl/bio.h>

namespace proxy {

// ── 내부 유틸리티 ───────────────────────────────────────────────────────────

/// OpenSSL 에러 큐에서 가장 최근 에러 메시지를 문자열로 반환
static std::string openssl_error_string() {
    unsigned long err = ERR_get_error();
    if (err == 0) return "unknown OpenSSL error";
    char buf[256];
    ERR_error_string_n(err, buf, sizeof(buf));
    return std::string(buf);
}

// ── 소멸자 / 이동 ────────────────────────────────────────────────────────────

MtlsContext::~MtlsContext() {
    if (ctx_) {
        SSL_CTX_free(ctx_);
        ctx_ = nullptr;
    }
}

MtlsContext::MtlsContext(MtlsContext&& other) noexcept
    : ctx_(other.ctx_) {
    other.ctx_ = nullptr;
}

MtlsContext& MtlsContext::operator=(MtlsContext&& other) noexcept {
    if (this != &other) {
        if (ctx_) SSL_CTX_free(ctx_);
        ctx_       = other.ctx_;
        other.ctx_ = nullptr;
    }
    return *this;
}

// ── 공통 SSL_CTX 초기화 ──────────────────────────────────────────────────────

SSL_CTX* MtlsContext::create_base_ctx(const std::string& cert_file,
                                      const std::string& key_file,
                                      const std::string& ca_file) {
    SSL_CTX* ctx = SSL_CTX_new(TLS_method());
    if (!ctx) {
        throw std::runtime_error("SSL_CTX_new failed: " + openssl_error_string());
    }

    // TLS 1.2 이상만 허용
    // TLS 1.0/1.1은 알려진 취약점(BEAST, POODLE 등)이 있어 Zero Trust 환경에서 부적합
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

    // 서버/클라이언트 인증서 + 개인키 로딩
    if (SSL_CTX_use_certificate_file(ctx, cert_file.c_str(),
                                     SSL_FILETYPE_PEM) != 1) {
        SSL_CTX_free(ctx);
        throw std::runtime_error("failed to load certificate '" + cert_file +
                                 "': " + openssl_error_string());
    }

    if (SSL_CTX_use_PrivateKey_file(ctx, key_file.c_str(),
                                    SSL_FILETYPE_PEM) != 1) {
        SSL_CTX_free(ctx);
        throw std::runtime_error("failed to load private key '" + key_file +
                                 "': " + openssl_error_string());
    }

    // 인증서와 개인키가 쌍을 이루는지 확인
    if (SSL_CTX_check_private_key(ctx) != 1) {
        SSL_CTX_free(ctx);
        throw std::runtime_error("certificate and private key do not match: " +
                                 openssl_error_string());
    }

    // CA 인증서 로딩 (피어 인증서 서명 검증에 사용)
    if (SSL_CTX_load_verify_locations(ctx, ca_file.c_str(), nullptr) != 1) {
        SSL_CTX_free(ctx);
        throw std::runtime_error("failed to load CA certificate '" + ca_file +
                                 "': " + openssl_error_string());
    }

    return ctx;
}

// ── 팩토리 함수 ─────────────────────────────────────────────────────────────

MtlsContext MtlsContext::create_server(const std::string& cert_file,
                                       const std::string& key_file,
                                       const std::string& ca_file) {
    SSL_CTX* ctx = create_base_ctx(cert_file, key_file, ca_file);

    // 서버 모드: 클라이언트 인증서를 요청하고 미제출 시 거부
    //
    // SSL_VERIFY_PEER:
    //   서버가 클라이언트에게 CertificateRequest 메시지를 전송.
    //   클라이언트가 인증서를 제시하면 CA 체인으로 검증.
    //
    // SSL_VERIFY_FAIL_IF_NO_PEER_CERT:
    //   클라이언트가 인증서를 전혀 보내지 않으면 핸드셰이크 실패.
    //   이 플래그 없이 SSL_VERIFY_PEER만 쓰면 인증서 미제출도 허용됨.
    SSL_CTX_set_verify(ctx,
                       SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
                       nullptr);

    // CA 검증 깊이 설정 (루트 CA → 중간 CA → 리프 인증서 최대 4단계)
    SSL_CTX_set_verify_depth(ctx, 4);

    return MtlsContext(ctx);
}

MtlsContext MtlsContext::create_client(const std::string& cert_file,
                                       const std::string& key_file,
                                       const std::string& ca_file) {
    SSL_CTX* ctx = create_base_ctx(cert_file, key_file, ca_file);

    // 클라이언트 모드: 서버 인증서 검증 활성화
    //
    // SSL_VERIFY_PEER: 서버 인증서를 CA로 검증.
    //                  검증 실패 시 SSL_connect()가 에러 반환.
    // SSL_VERIFY_FAIL_IF_NO_PEER_CERT는 클라이언트에서는 의미 없음
    //   (서버는 항상 인증서를 제시하므로)
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
    SSL_CTX_set_verify_depth(ctx, 4);

    return MtlsContext(ctx);
}

// ── 핸드셰이크 후 검증 ───────────────────────────────────────────────────────

bool MtlsContext::verify_peer(SSL* ssl, std::string* error_out) {
    // 1. CA 서명 + 유효기간 검증 결과 확인
    //
    //    OpenSSL은 핸드셰이크 중 자동으로 검증하지만,
    //    결과를 명시적으로 확인해 에러 메시지를 남기기 위해 재확인.
    long result = SSL_get_verify_result(ssl);
    if (result != X509_V_OK) {
        if (error_out) {
            *error_out = "certificate verification failed: " +
                         std::string(X509_verify_cert_error_string(result));
        }
        return false;
    }

    // 2. 피어가 인증서를 실제로 제출했는지 확인
    X509* cert = SSL_get_peer_certificate(ssl);
    if (!cert) {
        if (error_out) {
            *error_out = "peer did not present a certificate";
        }
        return false;
    }

    // 3. 인증서 유효 기간 재확인 (NotBefore ≤ now ≤ NotAfter)
    //
    //    SSL_get_verify_result()가 X509_V_OK면 이미 만료 확인이 포함되지만,
    //    일부 설정에서 시간 검증을 건너뛸 수 있으므로 이중 확인.
    const ASN1_TIME* not_before = X509_get0_notBefore(cert);
    const ASN1_TIME* not_after  = X509_get0_notAfter(cert);

    if (X509_cmp_current_time(not_before) > 0) {
        if (error_out) {
            *error_out = "certificate is not yet valid (NotBefore in future)";
        }
        X509_free(cert);
        return false;
    }

    if (X509_cmp_current_time(not_after) < 0) {
        if (error_out) {
            *error_out = "certificate has expired";
        }
        X509_free(cert);
        return false;
    }

    X509_free(cert);
    return true;
}

std::string MtlsContext::get_peer_common_name(SSL* ssl) {
    X509* cert = SSL_get_peer_certificate(ssl);
    if (!cert) return "";

    X509_NAME* subject = X509_get_subject_name(cert);
    if (!subject) {
        X509_free(cert);
        return "";
    }

    char cn_buf[256] = {};
    int  len = X509_NAME_get_text_by_NID(subject, NID_commonName,
                                          cn_buf, sizeof(cn_buf));
    X509_free(cert);

    if (len < 0) return "";
    return std::string(cn_buf, static_cast<size_t>(len));
}

std::string MtlsContext::get_cert_expiry(const std::string& cert_file) {
    BIO* bio = BIO_new_file(cert_file.c_str(), "r");
    if (!bio) {
        return "failed to open: " + cert_file;
    }

    X509* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);

    if (!cert) {
        return "failed to parse certificate: " + cert_file;
    }

    const ASN1_TIME* not_after = X509_get0_notAfter(cert);

    // ASN1_TIME → 사람이 읽을 수 있는 문자열로 변환
    BIO* mem = BIO_new(BIO_s_mem());
    ASN1_TIME_print(mem, not_after);

    char buf[128] = {};
    BIO_read(mem, buf, sizeof(buf) - 1);
    BIO_free(mem);
    X509_free(cert);

    return std::string(buf);
}

} // namespace proxy
