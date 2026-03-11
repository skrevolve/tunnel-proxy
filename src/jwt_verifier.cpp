#include "core/jwt_verifier.h"

#include <nlohmann/json.hpp>
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/bio.h>

#include <cstring>
#include <ctime>
#include <stdexcept>
#include <sstream>

namespace proxy {

// ── Base64url 인코딩/디코딩 ───────────────────────────────────────────────────

std::string JwtVerifier::base64url_encode(const std::vector<uint8_t>& data) {
    if (data.empty()) return "";

    BIO* b64 = BIO_new(BIO_f_base64());
    BIO* mem = BIO_new(BIO_s_mem());
    BIO_push(b64, mem);
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    BIO_write(b64, data.data(), static_cast<int>(data.size()));
    BIO_flush(b64);

    const char* ptr = nullptr;
    long len = BIO_get_mem_data(mem, &ptr);
    std::string result(ptr, static_cast<size_t>(len));
    BIO_free_all(b64);

    // Base64 → Base64url: '+' → '-', '/' → '_', '=' 제거
    for (char& c : result) {
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
    }
    while (!result.empty() && result.back() == '=') {
        result.pop_back();
    }
    return result;
}

std::vector<uint8_t> JwtVerifier::base64url_decode(const std::string& encoded) {
    // Base64url → Base64: '-' → '+', '_' → '/'
    std::string b64 = encoded;
    for (char& c : b64) {
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
    }
    // padding 보완
    switch (b64.size() % 4) {
        case 2: b64 += "=="; break;
        case 3: b64 += "=";  break;
        default: break;
    }

    BIO* b64_bio = BIO_new(BIO_f_base64());
    BIO* mem_bio = BIO_new_mem_buf(b64.data(), static_cast<int>(b64.size()));
    BIO_push(b64_bio, mem_bio);
    BIO_set_flags(b64_bio, BIO_FLAGS_BASE64_NO_NL);

    std::vector<uint8_t> result(b64.size());
    int decoded_len = BIO_read(b64_bio, result.data(), static_cast<int>(result.size()));
    BIO_free_all(b64_bio);

    if (decoded_len < 0) {
        throw std::runtime_error("base64url_decode: invalid base64 data");
    }
    result.resize(static_cast<size_t>(decoded_len));
    return result;
}

// ── Claims 만료 확인 ──────────────────────────────────────────────────────────

bool JwtVerifier::Claims::is_expired() const {
    if (expires == 0) return false;  // exp 없음 = 만료 없음
    return static_cast<int64_t>(std::time(nullptr)) > expires;
}

// ── Payload 파싱 ──────────────────────────────────────────────────────────────

JwtVerifier::Claims JwtVerifier::parse_claims(const std::string& payload_b64) {
    auto bytes = base64url_decode(payload_b64);
    std::string json_str(bytes.begin(), bytes.end());

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(json_str);
    } catch (const nlohmann::json::parse_error& e) {
        throw std::runtime_error(std::string("JWT: payload JSON parse error: ") + e.what());
    }

    Claims c;
    c.raw_payload = json_str;
    if (j.contains("sub") && j["sub"].is_string()) c.subject  = j["sub"].get<std::string>();
    if (j.contains("iss") && j["iss"].is_string()) c.issuer   = j["iss"].get<std::string>();
    if (j.contains("iat") && j["iat"].is_number()) c.issued_at = j["iat"].get<int64_t>();
    if (j.contains("exp") && j["exp"].is_number()) c.expires   = j["exp"].get<int64_t>();
    return c;
}

// ── 팩토리 함수 ───────────────────────────────────────────────────────────────

JwtVerifier JwtVerifier::create_hs256(const std::string& secret) {
    return JwtVerifier(Algorithm::HS256, secret);
}

JwtVerifier JwtVerifier::create_rs256(const std::string& public_key_pem) {
    // PEM 파싱 가능 여부 즉시 검증
    BIO* bio = BIO_new_mem_buf(public_key_pem.data(),
                               static_cast<int>(public_key_pem.size()));
    EVP_PKEY* pkey = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!pkey) {
        throw std::runtime_error("JWT RS256: invalid PEM public key");
    }
    EVP_PKEY_free(pkey);
    return JwtVerifier(Algorithm::RS256, public_key_pem);
}

// ── 서명 검증 ─────────────────────────────────────────────────────────────────

void JwtVerifier::verify_hs256(const std::string& message,
                                const std::string& sig_b64) const {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int  dlen = 0;
    HMAC(EVP_sha256(),
         key_data_.data(), static_cast<int>(key_data_.size()),
         reinterpret_cast<const unsigned char*>(message.data()),
         message.size(),
         digest, &dlen);

    std::vector<uint8_t> digest_vec(digest, digest + dlen);
    std::string expected_b64 = base64url_encode(digest_vec);

    // constant-time 비교 (timing-safe)
    if (expected_b64.size() != sig_b64.size() ||
        CRYPTO_memcmp(expected_b64.data(), sig_b64.data(), expected_b64.size()) != 0) {
        throw std::runtime_error("JWT HS256: signature mismatch");
    }
}

void JwtVerifier::verify_rs256(const std::string& message,
                                const std::string& sig_b64) const {
    BIO* bio = BIO_new_mem_buf(key_data_.data(),
                               static_cast<int>(key_data_.size()));
    EVP_PKEY* pkey = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!pkey) {
        throw std::runtime_error("JWT RS256: failed to load public key");
    }

    auto sig_bytes = base64url_decode(sig_b64);

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    int rc = EVP_DigestVerifyInit(ctx, nullptr, EVP_sha256(), nullptr, pkey);
    if (rc == 1) {
        rc = EVP_DigestVerifyUpdate(ctx,
                                    reinterpret_cast<const unsigned char*>(message.data()),
                                    message.size());
    }
    if (rc == 1) {
        rc = EVP_DigestVerifyFinal(ctx, sig_bytes.data(), sig_bytes.size());
    }
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);

    if (rc != 1) {
        throw std::runtime_error("JWT RS256: signature verification failed");
    }
}

// ── JWT 검증 메인 ─────────────────────────────────────────────────────────────

JwtVerifier::Claims JwtVerifier::verify(const std::string& token) const {
    // 1. "." 2개로 분리
    auto p1 = token.find('.');
    if (p1 == std::string::npos) {
        throw std::runtime_error("JWT: invalid format (missing first '.')");
    }
    auto p2 = token.find('.', p1 + 1);
    if (p2 == std::string::npos) {
        throw std::runtime_error("JWT: invalid format (missing second '.')");
    }

    std::string header_b64  = token.substr(0, p1);
    std::string payload_b64 = token.substr(p1 + 1, p2 - p1 - 1);
    std::string sig_b64     = token.substr(p2 + 1);
    std::string message     = header_b64 + "." + payload_b64;

    // 2. header alg 확인
    auto header_bytes = base64url_decode(header_b64);
    std::string header_json(header_bytes.begin(), header_bytes.end());
    nlohmann::json header;
    try {
        header = nlohmann::json::parse(header_json);
    } catch (...) {
        throw std::runtime_error("JWT: header JSON parse error");
    }
    if (!header.contains("alg") || !header["alg"].is_string()) {
        throw std::runtime_error("JWT: missing 'alg' in header");
    }
    std::string alg_str = header["alg"].get<std::string>();

    const std::string expected_alg = (alg_ == Algorithm::HS256) ? "HS256" : "RS256";
    if (alg_str != expected_alg) {
        throw std::runtime_error("JWT: algorithm mismatch — token=" + alg_str
                                 + " verifier=" + expected_alg);
    }

    // 3. 서명 검증
    if (alg_ == Algorithm::HS256) {
        verify_hs256(message, sig_b64);
    } else {
        verify_rs256(message, sig_b64);
    }

    // 4. Claims 파싱 + 만료 확인
    Claims claims = parse_claims(payload_b64);
    if (claims.is_expired()) {
        throw std::runtime_error("JWT: token expired");
    }
    return claims;
}

// ── 유틸리티 ──────────────────────────────────────────────────────────────────

std::string JwtVerifier::extract_bearer_token(const std::string& auth_header) {
    const std::string prefix = "Bearer ";
    if (auth_header.size() <= prefix.size()) return "";
    if (auth_header.substr(0, prefix.size()) != prefix) return "";
    return auth_header.substr(prefix.size());
}

} // namespace proxy
