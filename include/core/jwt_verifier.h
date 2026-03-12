#pragma once

#include <string>
#include <cstdint>
#include <vector>

namespace proxy {

/**
 * @file jwt_verifier.h
 * @brief Phase 7-B — JWT (JSON Web Token) 파싱 + 서명 검증
 *
 * ── mTLS vs JWT ───────────────────────────────────────────────────────────────
 *
 *   mTLS (Phase 7-A): 장치/에이전트 인증
 *     - 인증서를 TLS 핸드셰이크에서 교환. 네트워크 연결 단계에서 차단.
 *     - "이 머신/에이전트가 등록된 것인가?"
 *
 *   JWT (Phase 7-B): 사용자/서비스 인증
 *     - HTTP 요청 헤더에 토큰 첨부. 애플리케이션 레이어에서 검증.
 *     - "이 사용자/서비스가 접근 권한을 가진 것인가?"
 *
 * ── JWT 구조 ──────────────────────────────────────────────────────────────────
 *
 *   JWT = Base64url(header) + "." + Base64url(payload) + "." + Base64url(sig)
 *
 *   header  (JSON): {"alg": "HS256", "typ": "JWT"}
 *   payload (JSON): {"sub": "user", "iss": "server", "iat": 1700000000, "exp": 1700003600}
 *   sig:            HMAC-SHA256(header+"."+payload, secret)  — HS256
 *              or   RSA-SHA256_sign(header+"."+payload, private_key)  — RS256
 *
 * ── HS256 vs RS256 ────────────────────────────────────────────────────────────
 *
 *   HS256 (HMAC-SHA256):
 *     공유 비밀키 하나로 서명+검증.
 *     서명자와 검증자가 같은 키를 알아야 함.
 *     단순하고 빠르지만 비밀키를 여러 서비스가 공유해야 함.
 *     → 단일 서버 환경, 내부 서비스 간 인증에 적합.
 *
 *   RS256 (RSA-SHA256):
 *     개인키로 서명, 공개키로 검증.
 *     검증자는 공개키만 알면 됨 → 비밀키 없이도 검증 가능.
 *     → 분산 환경, IdP(Identity Provider)가 토큰을 발급하는 경우 적합.
 *     인증 서버가 개인키로 서명, 터널 서버는 공개키로 검증.
 *
 * ── 표준 Claims (RFC 7519) ────────────────────────────────────────────────────
 *
 *   "sub" (Subject):   토큰 주체 (사용자 ID, 에이전트 ID 등)
 *   "iss" (Issuer):    토큰 발급자
 *   "iat" (Issued At): 발급 시각 (Unix timestamp)
 *   "exp" (Expiration): 만료 시각 (Unix timestamp). 이 값이 없으면 영구 유효.
 *
 * ── 사용 예시 ─────────────────────────────────────────────────────────────────
 *
 *   // HS256
 *   auto v = JwtVerifier::create_hs256("my-secret-key");
 *   auto claims = v.verify("eyJ...토큰...");
 *   // → 서명 불일치, 만료, 형식 오류 시 std::runtime_error 예외
 *
 *   // RS256
 *   auto v = JwtVerifier::create_rs256(public_key_pem);
 *   auto claims = v.verify(token);
 *
 *   // Authorization 헤더에서 토큰 추출
 *   auto token = JwtVerifier::extract_bearer_token("Bearer eyJ...");
 */
class JwtVerifier {
public:
    /**
     * 파싱된 JWT Claims
     *
     * raw_payload: 원본 payload JSON 문자열.
     *              표준 Claims 이외의 커스텀 필드 접근 시 직접 파싱해서 사용.
     */
    struct Claims {
        std::string subject;       // "sub"
        std::string issuer;        // "iss"
        int64_t     issued_at{0};  // "iat" (0이면 미설정)
        int64_t     expires{0};    // "exp" (0이면 만료 없음)
        std::string raw_payload;   // 원본 payload JSON

        /** 현재 시각 기준 만료 여부. expires==0이면 항상 false (만료 없음). */
        bool is_expired() const;
    };

    // ── 팩토리 함수 ─────────────────────────────────────────────────────────

    /**
     * HS256 (HMAC-SHA256) 검증기 생성
     *
     * @param secret 서명/검증에 사용하는 공유 비밀키 (임의 길이 문자열)
     */
    static JwtVerifier create_hs256(const std::string& secret);

    /**
     * RS256 (RSA-SHA256) 검증기 생성
     *
     * @param public_key_pem RSA 공개키 (PEM 형식, "-----BEGIN PUBLIC KEY-----" 포함)
     * @throws std::runtime_error PEM 파싱 실패 시
     */
    static JwtVerifier create_rs256(const std::string& public_key_pem);

    // ── 검증 ────────────────────────────────────────────────────────────────

    /**
     * JWT 검증: 형식 파싱 → alg 확인 → 서명 검증 → 만료 확인 → Claims 반환
     *
     * 실패 조건 (std::runtime_error 예외):
     *   - "." 2개로 분리되지 않음 (형식 오류)
     *   - header의 "alg" 필드가 생성 시 지정한 알고리즘과 불일치
     *   - Base64url 디코딩 실패
     *   - 서명 불일치 (HMAC 불일치 또는 RSA 검증 실패)
     *   - "exp" claim이 현재 시각보다 과거
     *
     * @param token "header.payload.signature" 형식의 JWT 문자열
     * @return 검증된 Claims
     */
    Claims verify(const std::string& token) const;

    // ── 유틸리티 함수 ────────────────────────────────────────────────────────

    /**
     * HTTP Authorization 헤더에서 Bearer 토큰 추출
     *
     * "Bearer eyJhbGci..." → "eyJhbGci..."
     * "Bearer " 접두사가 없으면 빈 문자열 반환.
     *
     * @param auth_header Authorization 헤더 값 전체
     */
    static std::string extract_bearer_token(const std::string& auth_header);

    /**
     * Base64url 인코딩 (RFC 4648 §5)
     *
     * '+' → '-', '/' → '_', '=' padding 생략.
     * HS256 토큰 생성 유틸리티로도 사용 가능.
     */
    static std::string base64url_encode(const std::vector<uint8_t>& data);

    /**
     * Base64url 디코딩
     *
     * '-' → '+', '_' → '/', padding 자동 보완.
     * @throws std::runtime_error 잘못된 Base64 문자 포함 시
     */
    static std::vector<uint8_t> base64url_decode(const std::string& encoded);

private:
    enum class Algorithm { HS256, RS256 };

    JwtVerifier(Algorithm alg, std::string key_data)
        : alg_(alg), key_data_(std::move(key_data)) {}

    /**
     * HS256 서명 검증
     *
     * HMAC-SHA256(header_b64 + "." + payload_b64, secret) 재계산 후
     * 토큰의 signature와 상수 시간 비교 (timing-safe compare).
     *
     * @throws std::runtime_error 서명 불일치
     */
    void verify_hs256(const std::string& message,
                      const std::string& sig_b64) const;

    /**
     * RS256 서명 검증
     *
     * PEM 공개키 로딩 → EVP_DigestVerify로 SHA256-RSA 검증.
     *
     * @throws std::runtime_error 서명 불일치 또는 키 오류
     */
    void verify_rs256(const std::string& message,
                      const std::string& sig_b64) const;

    /**
     * payload Base64url → JSON 파싱 → Claims 구조체
     *
     * @throws std::runtime_error JSON 파싱 실패
     */
    static Claims parse_claims(const std::string& payload_b64);

    Algorithm   alg_;
    std::string key_data_;  // HS256: secret 문자열, RS256: PEM 공개키
};

} // namespace proxy
