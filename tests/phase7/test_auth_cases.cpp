#include "core/jwt_verifier.h"
#include "core/access_policy.h"

#include <gtest/gtest.h>
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/bio.h>

#include <ctime>
#include <string>
#include <vector>
#include <fstream>

using namespace proxy;

// ── JWT 토큰 생성 헬퍼 ─────────────────────────────────────────────────────────

static std::string make_hs256_token(const std::string& secret,
                                    const std::string& payload_json,
                                    bool tamper_sig = false) {
    auto b64 = [](const std::vector<uint8_t>& d) {
        return JwtVerifier::base64url_encode(d);
    };
    auto b64s = [&](const std::string& s) {
        return b64(std::vector<uint8_t>(s.begin(), s.end()));
    };

    std::string header_b64  = b64s(R"({"alg":"HS256","typ":"JWT"})");
    std::string payload_b64 = b64s(payload_json);
    std::string message     = header_b64 + "." + payload_b64;

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int dlen = 0;
    HMAC(EVP_sha256(),
         secret.data(), static_cast<int>(secret.size()),
         reinterpret_cast<const unsigned char*>(message.data()), message.size(),
         digest, &dlen);

    std::vector<uint8_t> sig_vec(digest, digest + dlen);
    if (tamper_sig) sig_vec.back() ^= 0xFF;  // 마지막 바이트 변조
    return message + "." + b64(sig_vec);
}


// 테스트용 RSA 키 쌍 생성 (런타임)
struct RsaKeyPair {
    std::string private_pem;
    std::string public_pem;
};

static RsaKeyPair generate_rsa_keypair() {
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    EVP_PKEY_keygen_init(ctx);
    EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 2048);
    EVP_PKEY* pkey = nullptr;
    EVP_PKEY_keygen(ctx, &pkey);
    EVP_PKEY_CTX_free(ctx);

    // private key PEM
    BIO* bio = BIO_new(BIO_s_mem());
    PEM_write_bio_PrivateKey(bio, pkey, nullptr, nullptr, 0, nullptr, nullptr);
    const char* ptr; long len = BIO_get_mem_data(bio, &ptr);
    std::string priv(ptr, len);
    BIO_free(bio);

    // public key PEM
    bio = BIO_new(BIO_s_mem());
    PEM_write_bio_PUBKEY(bio, pkey);
    len = BIO_get_mem_data(bio, &ptr);
    std::string pub(ptr, len);
    BIO_free(bio);

    EVP_PKEY_free(pkey);
    return {priv, pub};
}

static std::string make_rs256_token(const std::string& private_pem,
                                    const std::string& payload_json) {
    auto b64s = [](const std::string& s) {
        return JwtVerifier::base64url_encode(
            std::vector<uint8_t>(s.begin(), s.end()));
    };

    std::string header_b64  = b64s(R"({"alg":"RS256","typ":"JWT"})");
    std::string payload_b64 = b64s(payload_json);
    std::string message     = header_b64 + "." + payload_b64;

    BIO* bio = BIO_new_mem_buf(private_pem.data(),
                               static_cast<int>(private_pem.size()));
    EVP_PKEY* pkey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);

    EVP_MD_CTX* mdctx = EVP_MD_CTX_new();
    EVP_DigestSignInit(mdctx, nullptr, EVP_sha256(), nullptr, pkey);
    EVP_DigestSignUpdate(mdctx,
        reinterpret_cast<const unsigned char*>(message.data()), message.size());
    size_t siglen = 0;
    EVP_DigestSignFinal(mdctx, nullptr, &siglen);
    std::vector<uint8_t> sig(siglen);
    EVP_DigestSignFinal(mdctx, sig.data(), &siglen);
    sig.resize(siglen);
    EVP_MD_CTX_free(mdctx);
    EVP_PKEY_free(pkey);

    return message + "." + JwtVerifier::base64url_encode(sig);
}

static std::string payload_with_exp(int64_t exp,
                                    const std::string& sub = "alice",
                                    const std::string& iss = "test") {
    return R"({"sub":")" + sub + R"(","iss":")" + iss +
           R"(","iat":1700000000,"exp":)" + std::to_string(exp) + "}";
}

static std::string payload_no_exp(const std::string& sub = "alice") {
    return R"({"sub":")" + sub + R"(","iss":"test","iat":1700000000})";
}

// ── 공유 키 쌍 (테스트 픽스처) ────────────────────────────────────────────────

class JwtTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        kp = generate_rsa_keypair();
    }
    static RsaKeyPair kp;

    const std::string secret = "test-secret-key";
    const int64_t future_exp = static_cast<int64_t>(std::time(nullptr)) + 3600;
    const int64_t past_exp   = static_cast<int64_t>(std::time(nullptr)) - 1;
};
RsaKeyPair JwtTest::kp;

// ═════════════════════════════════════════════════════════════════════════════
// JWT 시나리오 1: 유효한 HS256 토큰 — 정상 검증 + Claims 확인
// ═════════════════════════════════════════════════════════════════════════════
TEST_F(JwtTest, HS256_ValidToken_ReturnsClaims) {
    std::string token = make_hs256_token(secret,
        payload_with_exp(future_exp, "alice", "myserver"));
    auto v = JwtVerifier::create_hs256(secret);
    auto claims = v.verify(token);

    EXPECT_EQ(claims.subject, "alice");
    EXPECT_EQ(claims.issuer,  "myserver");
    EXPECT_EQ(claims.issued_at, 1700000000);
    EXPECT_EQ(claims.expires,   future_exp);
    EXPECT_FALSE(claims.is_expired());
}

// ═════════════════════════════════════════════════════════════════════════════
// JWT 시나리오 2: 유효한 RS256 토큰 — RSA 공개키로 정상 검증
// ═════════════════════════════════════════════════════════════════════════════
TEST_F(JwtTest, RS256_ValidToken_ReturnsClaims) {
    std::string token = make_rs256_token(kp.private_pem,
        payload_with_exp(future_exp, "bob", "auth-server"));
    auto v = JwtVerifier::create_rs256(kp.public_pem);
    auto claims = v.verify(token);

    EXPECT_EQ(claims.subject, "bob");
    EXPECT_EQ(claims.issuer,  "auth-server");
    EXPECT_FALSE(claims.is_expired());
}

// ═════════════════════════════════════════════════════════════════════════════
// JWT 시나리오 3: 만료된 토큰 — runtime_error 발생
// ═════════════════════════════════════════════════════════════════════════════
TEST_F(JwtTest, HS256_ExpiredToken_Throws) {
    std::string token = make_hs256_token(secret, payload_with_exp(past_exp));
    auto v = JwtVerifier::create_hs256(secret);
    EXPECT_THROW(v.verify(token), std::runtime_error);
}

// ═════════════════════════════════════════════════════════════════════════════
// JWT 시나리오 4: 위조된 서명 — runtime_error 발생
// ═════════════════════════════════════════════════════════════════════════════
TEST_F(JwtTest, HS256_TamperedSignature_Throws) {
    std::string token = make_hs256_token(secret,
        payload_with_exp(future_exp), /*tamper_sig=*/true);
    auto v = JwtVerifier::create_hs256(secret);
    EXPECT_THROW(v.verify(token), std::runtime_error);
}

// ═════════════════════════════════════════════════════════════════════════════
// JWT 시나리오 5: 알고리즘 불일치 — runtime_error 발생
// ═════════════════════════════════════════════════════════════════════════════
TEST_F(JwtTest, AlgorithmMismatch_HS256VerifierRS256Token_Throws) {
    // RS256 토큰을 HS256 검증기로 검증
    std::string token = make_rs256_token(kp.private_pem,
        payload_with_exp(future_exp));
    auto v = JwtVerifier::create_hs256(secret);
    EXPECT_THROW(v.verify(token), std::runtime_error);
}

// ═════════════════════════════════════════════════════════════════════════════
// JWT 시나리오 6: 형식 오류 — runtime_error 발생
// ═════════════════════════════════════════════════════════════════════════════
TEST_F(JwtTest, MalformedToken_NoDots_Throws) {
    auto v = JwtVerifier::create_hs256(secret);
    EXPECT_THROW(v.verify("notavalidtoken"), std::runtime_error);
}

TEST_F(JwtTest, MalformedToken_OneDot_Throws) {
    auto v = JwtVerifier::create_hs256(secret);
    EXPECT_THROW(v.verify("header.payload"), std::runtime_error);
}

// ═════════════════════════════════════════════════════════════════════════════
// JWT 시나리오 7: exp 없는 토큰 — 만료 없음으로 정상 통과
// ═════════════════════════════════════════════════════════════════════════════
TEST_F(JwtTest, HS256_NoExpClaim_PassesWithoutExpiry) {
    std::string token = make_hs256_token(secret, payload_no_exp("charlie"));
    auto v = JwtVerifier::create_hs256(secret);
    auto claims = v.verify(token);

    EXPECT_EQ(claims.subject, "charlie");
    EXPECT_EQ(claims.expires, 0);
    EXPECT_FALSE(claims.is_expired());
}

// ═════════════════════════════════════════════════════════════════════════════
// JWT 시나리오 8: extract_bearer_token 파싱
// ═════════════════════════════════════════════════════════════════════════════
TEST_F(JwtTest, ExtractBearerToken_ValidPrefix_ReturnsToken) {
    EXPECT_EQ(JwtVerifier::extract_bearer_token("Bearer mytoken123"),
              "mytoken123");
}

TEST_F(JwtTest, ExtractBearerToken_NoPrefix_ReturnsEmpty) {
    EXPECT_EQ(JwtVerifier::extract_bearer_token("mytoken123"), "");
    EXPECT_EQ(JwtVerifier::extract_bearer_token(""), "");
}

// ═════════════════════════════════════════════════════════════════════════════
// JWT 시나리오 16: create_rs256() — 잘못된 PEM 시 객체 생성 즉시 runtime_error
// ═════════════════════════════════════════════════════════════════════════════
TEST_F(JwtTest, RS256_InvalidPem_ThrowsOnCreate) {
    EXPECT_THROW(JwtVerifier::create_rs256("not-a-pem"), std::runtime_error);
    EXPECT_THROW(JwtVerifier::create_rs256(""), std::runtime_error);
}

// ═════════════════════════════════════════════════════════════════════════════
// AccessPolicy 시나리오
// ═════════════════════════════════════════════════════════════════════════════

class PolicyTest : public ::testing::Test {
protected:
    AccessPolicy policy_with_rules() {
        AccessPolicy p;
        p.add_rule({"alice", "agent-1", "192.168.1.10", 22});
        p.add_rule({"bob",   "*",       "*",            0 });
        return p;
    }
};

// ─── 시나리오 9: 정확히 매칭되는 규칙 → true ─────────────────────────────────
TEST_F(PolicyTest, ExactMatch_ReturnsTrue) {
    auto p = policy_with_rules();
    EXPECT_TRUE(p.is_allowed("alice", "agent-1", "192.168.1.10", 22));
}

// ─── 시나리오 10: 필드 하나라도 불일치 → false ───────────────────────────────
TEST_F(PolicyTest, SubjectMismatch_ReturnsFalse) {
    auto p = policy_with_rules();
    EXPECT_FALSE(p.is_allowed("carol", "agent-1", "192.168.1.10", 22));
}

TEST_F(PolicyTest, TunnelMismatch_ReturnsFalse) {
    auto p = policy_with_rules();
    EXPECT_FALSE(p.is_allowed("alice", "agent-2", "192.168.1.10", 22));
}

TEST_F(PolicyTest, IpMismatch_ReturnsFalse) {
    auto p = policy_with_rules();
    EXPECT_FALSE(p.is_allowed("alice", "agent-1", "192.168.1.99", 22));
}

TEST_F(PolicyTest, PortMismatch_ReturnsFalse) {
    auto p = policy_with_rules();
    EXPECT_FALSE(p.is_allowed("alice", "agent-1", "192.168.1.10", 80));
}

// ─── 시나리오 11: wildcard subject "*" → 다른 사용자도 허용 ───────────────────
TEST_F(PolicyTest, WildcardSubject_AnyUserAllowed) {
    AccessPolicy p;
    p.add_rule({"*", "agent-1", "10.0.0.1", 80});
    EXPECT_TRUE(p.is_allowed("alice", "agent-1", "10.0.0.1", 80));
    EXPECT_TRUE(p.is_allowed("bob",   "agent-1", "10.0.0.1", 80));
    EXPECT_TRUE(p.is_allowed("carol", "agent-1", "10.0.0.1", 80));
}

// ─── 시나리오 12: 완전 wildcard 규칙 → 모든 요청 허용 ───────────────────────
TEST_F(PolicyTest, FullWildcard_AllowsEverything) {
    AccessPolicy p;
    p.add_rule({"*", "*", "*", 0});
    EXPECT_TRUE(p.is_allowed("alice", "agent-1", "192.168.1.1", 22));
    EXPECT_TRUE(p.is_allowed("bob",   "agent-2", "10.0.0.5",    3306));
    EXPECT_TRUE(p.is_allowed("",      "",        "",            65535));
}

// ─── 시나리오 13: first-match 순서 ───────────────────────────────────────────
TEST_F(PolicyTest, FirstMatch_EarlyRuleTakesPrecedence) {
    AccessPolicy p;
    // alice에 특정 규칙 먼저, 그 다음 wildcard
    p.add_rule({"alice", "agent-1", "192.168.1.10", 22});
    p.add_rule({"*",     "*",       "*",            0 });

    // alice는 첫 번째 규칙으로 agent-1:22만 명시적 허용 → wildcard도 있으므로 다른 것도 허용
    EXPECT_TRUE(p.is_allowed("alice", "agent-1", "192.168.1.10", 22));
    // wildcard 규칙 덕분에 carol도 허용
    EXPECT_TRUE(p.is_allowed("carol", "agent-2", "10.0.0.1", 80));
}

TEST_F(PolicyTest, FirstMatch_DenyBeforeWildcard) {
    // wildcard보다 앞에 특정 규칙이 없으면 wildcard가 매칭됨을 확인
    // (deny 기능은 없으므로, 규칙이 없는 경우를 통해 first-match 동작 검증)
    AccessPolicy p;
    p.add_rule({"alice", "agent-1", "192.168.1.10", 22});
    // bob에 대한 규칙 없음 → default-deny
    EXPECT_FALSE(p.is_allowed("bob", "agent-1", "192.168.1.10", 22));
    // alice만 허용
    EXPECT_TRUE(p.is_allowed("alice", "agent-1", "192.168.1.10", 22));
}

// ─── 시나리오 14: 빈 정책 → default-deny ────────────────────────────────────
TEST_F(PolicyTest, EmptyPolicy_DefaultDeny) {
    AccessPolicy p;
    EXPECT_EQ(p.rule_count(), 0u);
    EXPECT_FALSE(p.is_allowed("alice", "agent-1", "192.168.1.10", 22));
}

// ─── 시나리오 15: JSON 파싱 + 필수 필드 누락 시 runtime_error ─────────────────
TEST_F(PolicyTest, LoadFromString_ValidJson_ParsesCorrectly) {
    const char* json = R"({
        "rules": [
            {"subject":"alice","tunnel_id":"agent-1","target_ip":"10.0.0.1","target_port":22},
            {"subject":"*","tunnel_id":"*","target_ip":"*","target_port":0}
        ]
    })";
    auto p = AccessPolicy::load_from_string(json);
    EXPECT_EQ(p.rule_count(), 2u);
    EXPECT_TRUE(p.is_allowed("alice", "agent-1", "10.0.0.1", 22));
    EXPECT_TRUE(p.is_allowed("bob",   "agent-2", "10.0.0.2", 80));
}

TEST_F(PolicyTest, LoadFromString_MissingSubject_Throws) {
    const char* json = R"({"rules":[{"tunnel_id":"a","target_ip":"1.2.3.4","target_port":22}]})";
    EXPECT_THROW(AccessPolicy::load_from_string(json), std::runtime_error);
}

TEST_F(PolicyTest, LoadFromString_MissingRulesKey_Throws) {
    EXPECT_THROW(AccessPolicy::load_from_string(R"({"policies":[]})"),
                 std::runtime_error);
}

TEST_F(PolicyTest, LoadFromString_InvalidJson_Throws) {
    EXPECT_THROW(AccessPolicy::load_from_string("not-json"), std::runtime_error);
}

// ─── 시나리오 17: load_from_file() — 존재하지 않는 파일 → runtime_error ───────
TEST_F(PolicyTest, LoadFromFile_NonExistentPath_Throws) {
    EXPECT_THROW(AccessPolicy::load_from_file("/tmp/no_such_file_xyz.json"),
                 std::runtime_error);
}

TEST_F(PolicyTest, LoadFromFile_ValidFile_ParsesCorrectly) {
    const std::string path = "/tmp/test_policy_phase7.json";
    {
        std::ofstream f(path);
        f << R"({"rules":[{"subject":"alice","tunnel_id":"agent-1","target_ip":"10.0.0.1","target_port":22}]})";
    }
    auto p = AccessPolicy::load_from_file(path);
    EXPECT_EQ(p.rule_count(), 1u);
    EXPECT_TRUE(p.is_allowed("alice", "agent-1", "10.0.0.1", 22));
    std::remove(path.c_str());
}
