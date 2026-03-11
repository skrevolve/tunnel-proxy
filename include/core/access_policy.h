#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace proxy {

/**
 * @file access_policy.h
 * @brief Phase 7-C — 접근 제어 정책 (터널별 / 사용자별 허용 규칙)
 *
 * ── 인증 vs 접근 제어 ────────────────────────────────────────────────────────
 *
 *   인증 (Phase 7-A/B): "누구인가?" 확인
 *     - mTLS: 장치/에이전트 신원 확인
 *     - JWT:  사용자/서비스 신원 확인
 *
 *   접근 제어 (Phase 7-C): "무엇에 접근할 수 있는가?" 결정
 *     - 인증된 사용자라도 허용된 터널/리소스만 접근 가능
 *     - Zero Trust의 핵심: 네트워크 위치뿐 아니라 매 요청마다 권한 확인
 *
 * ── 정책 규칙 구조 ──────────────────────────────────────────────────────────
 *
 *   Rule:
 *     subject    — JWT sub 값 (사용자/서비스 ID). "*" = 모든 주체 허용
 *     tunnel_id  — TunnelAgent agent_id.          "*" = 모든 터널 허용
 *     target_ip  — 에이전트가 연결할 내부 서버 IP. "*" = 모든 IP 허용
 *     target_port — 내부 서버 포트. 0 = 모든 포트 허용
 *
 *   매칭 방식: 모든 필드가 일치하면 허용 (AND 조건).
 *   규칙 목록을 순서대로 검사해 첫 번째 매칭 규칙이 결과를 결정 (first-match).
 *   매칭 규칙이 없으면 기본적으로 거부 (default-deny).
 *
 * ── 정책 파일 형식 (JSON) ───────────────────────────────────────────────────
 *
 *   {
 *     "rules": [
 *       { "subject": "alice",   "tunnel_id": "agent-1", "target_ip": "192.168.1.10", "target_port": 22 },
 *       { "subject": "bob",     "tunnel_id": "*",       "target_ip": "*",            "target_port": 0  },
 *       { "subject": "*",       "tunnel_id": "agent-2", "target_ip": "10.0.0.5",     "target_port": 80 }
 *     ]
 *   }
 *
 *   - "alice"는 agent-1의 192.168.1.10:22만 접근 가능
 *   - "bob"은 모든 터널, 모든 타겟 접근 가능 (관리자)
 *   - 누구나 agent-2의 10.0.0.5:80 접근 가능
 *
 * ── 사용 예시 ────────────────────────────────────────────────────────────────
 *
 *   // 파일에서 로딩
 *   auto policy = AccessPolicy::load_from_file("policy.json");
 *
 *   // 인라인 규칙 추가
 *   AccessPolicy policy;
 *   policy.add_rule({"alice", "agent-1", "192.168.1.10", 22});
 *   policy.add_rule({"*",     "*",       "*",            0 });
 *
 *   // JWT 검증 후 접근 제어
 *   auto claims = verifier.verify(token);
 *   if (!policy.is_allowed(claims.subject, agent_id, target_ip, target_port)) {
 *       throw std::runtime_error("access denied");
 *   }
 */
class AccessPolicy {
public:
    /**
     * 단일 접근 허용 규칙
     *
     * 필드 값이 "*"이면 해당 필드는 모든 값에 매칭.
     * target_port가 0이면 모든 포트에 매칭.
     */
    struct Rule {
        std::string subject;      // JWT sub ("*" = 모두)
        std::string tunnel_id;    // agent_id  ("*" = 모두)
        std::string target_ip;    // 내부 서버 IP ("*" = 모두)
        uint16_t    target_port;  // 내부 서버 포트 (0 = 모두)
    };

    AccessPolicy() = default;

    // ── 규칙 관리 ────────────────────────────────────────────────────────────

    /**
     * 규칙 추가 (목록 맨 끝에 append)
     *
     * 규칙은 추가된 순서대로 평가된다 (first-match).
     */
    void add_rule(Rule rule);

    /**
     * JSON 파일에서 정책 로딩
     *
     * @param path JSON 파일 경로
     * @throws std::runtime_error 파일 읽기 실패 또는 JSON 파싱 오류
     */
    static AccessPolicy load_from_file(const std::string& path);

    /**
     * JSON 문자열에서 정책 파싱
     *
     * 테스트 및 인라인 설정에 사용.
     * @throws std::runtime_error JSON 파싱 오류
     */
    static AccessPolicy load_from_string(const std::string& json_str);

    // ── 접근 제어 판단 ───────────────────────────────────────────────────────

    /**
     * 접근 허용 여부 판단 (first-match, default-deny)
     *
     * 규칙 목록을 순서대로 검사해 모든 필드가 매칭되는 첫 번째 규칙이
     * 결과를 결정한다. 매칭 규칙이 없으면 false(거부).
     *
     * @param subject     JWT Claims.subject (사용자/서비스 ID)
     * @param tunnel_id   TunnelAgent의 agent_id
     * @param target_ip   에이전트가 연결할 내부 서버 IP
     * @param target_port 에이전트가 연결할 내부 서버 포트
     * @return true = 허용, false = 거부
     */
    bool is_allowed(const std::string& subject,
                    const std::string& tunnel_id,
                    const std::string& target_ip,
                    uint16_t           target_port) const;

    // ── 상태 조회 ────────────────────────────────────────────────────────────

    /** 현재 등록된 규칙 수 */
    size_t rule_count() const { return rules_.size(); }

    /** 전체 규칙 목록 (읽기 전용) */
    const std::vector<Rule>& rules() const { return rules_; }

private:
    /**
     * 단일 규칙과 요청 파라미터의 매칭 여부
     *
     * 모든 필드가 매칭되어야 true.
     * 필드값 "*" 또는 port==0 은 해당 필드를 항상 매칭으로 처리.
     */
    static bool matches(const Rule&        rule,
                        const std::string& subject,
                        const std::string& tunnel_id,
                        const std::string& target_ip,
                        uint16_t           target_port);

    /**
     * JSON 오브젝트 배열을 Rules로 변환 (공통 파싱 로직)
     * @throws std::runtime_error JSON 구조 오류
     */
    static AccessPolicy parse_json(const std::string& json_str);

    std::vector<Rule> rules_;
};

} // namespace proxy
