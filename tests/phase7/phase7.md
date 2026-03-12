# Phase 7 테스트 가이드

Phase 6까지는 터널이 연결되면 데이터를 그냥 흘렸다.
Phase 7은 **인증 없이는 아무것도 허용하지 않는다**는 Zero Trust를 검증한다.
세 가지 레이어를 검증한다: mTLS(장치 인증) → JWT(사용자 인증) → AccessPolicy(리소스 접근 제어).

---

## 인증 레이어 구조

```
연결 요청
  ↓
[1] mTLS — CA 서명 클라이언트 인증서 검증 (장치)
  ↓
[2] JWT  — Bearer 토큰 서명/만료 검증 (사용자)
  ↓
[3] AccessPolicy — subject/tunnel/ip/port 접근 규칙 (리소스)
  ↓
허용 or 차단
```

---

## Phase 7-A — mTLS 컨텍스트 (`test_mtls.cpp`)

### 시나리오 1 — 유효한 CA 서명 인증서로 SSL_CTX 생성

같은 CA가 서명한 서버/클라이언트 인증서 세트로 `create_server()` / `create_client()`가 정상 생성되는지 확인한다.

```bash
ctest -R "MtlsContextTest/CreateServerAndClient_ValidCerts_Succeeds" --output-on-failure
```

**예상 출력**:

```
[ RUN      ] MtlsContextTest.CreateServerAndClient_ValidCerts_Succeeds
[       OK ] MtlsContextTest.CreateServerAndClient_ValidCerts_Succeeds
```

**검증**: `EXPECT_NO_THROW` — `SSL_CTX_use_certificate_file` + `SSL_CTX_use_PrivateKey_file` + `SSL_CTX_load_verify_locations` 모두 성공.

---

### 시나리오 2 — socketpair 핸드셰이크 후 CN 추출

실제 mTLS 핸드셰이크(socketpair + 스레드) 후 `get_peer_common_name()`이 클라이언트 인증서 CN을 정확히 반환하는지 확인한다.
CN은 Phase 7-C 접근 정책에서 subject로 사용되므로, 파싱 오류는 모든 접근 제어를 망가뜨린다.

```bash
ctest -R "MtlsContextTest/GetPeerCommonName_ReturnsClientCN" --output-on-failure
```

**예상 출력**:

```
[ RUN      ] MtlsContextTest.GetPeerCommonName_ReturnsClientCN
[       OK ] MtlsContextTest.GetPeerCommonName_ReturnsClientCN
```

**검증**: `SSL_accept` 성공 → `get_peer_common_name(ssl) == "my-tunnel-agent"`.

---

### 시나리오 3 — get_cert_expiry 만료일 반환

`get_cert_expiry(cert_path)` 가 인증서 파일 경로를 받아 비어있지 않은 만료일 문자열을 반환하는지 확인한다.

```bash
ctest -R "MtlsContextTest/GetCertExpiry_ReturnsNonEmptyString" --output-on-failure
```

**예상 출력**:

```
[ RUN      ] MtlsContextTest.GetCertExpiry_ReturnsNonEmptyString
[       OK ] MtlsContextTest.GetCertExpiry_ReturnsNonEmptyString
```

**검증**: `expiry.empty() == false`.

---

### 시나리오 4 — 잘못된 인증서/키 경로 → 예외

존재하지 않는 cert 또는 key 경로로 `create_server()`를 호출하면 즉시 예외를 던져야 한다.
잘못된 경로를 묵인하면 핸드셰이크 시점까지 오류가 지연되어 디버깅이 어려워진다.

```bash
ctest -R "MtlsContextTest/CreateServer_MissingCertFile_Throws" --output-on-failure
ctest -R "MtlsContextTest/CreateServer_MissingKeyFile_Throws" --output-on-failure
```

**예상 출력**:

```
[ RUN      ] MtlsContextTest.CreateServer_MissingCertFile_Throws
[       OK ] MtlsContextTest.CreateServer_MissingCertFile_Throws
[ RUN      ] MtlsContextTest.CreateServer_MissingKeyFile_Throws
[       OK ] MtlsContextTest.CreateServer_MissingKeyFile_Throws
```

**검증**: `/nonexistent/cert.crt` 또는 `/nonexistent/server.key` → `std::runtime_error`.

---

### 시나리오 5 — 신뢰하지 않는 CA 인증서 → 핸드셰이크 실패

서버가 신뢰하는 CA와 다른 CA가 서명한 클라이언트 인증서를 제시하면 `verify_peer()`가 false를 반환해야 한다.
CA 체인 검증이 없으면 자체 서명 인증서로 mTLS를 우회할 수 있다.

```bash
ctest -R "MtlsContextTest/VerifyPeer_SelfSignedClientWithoutCA_Fails" --output-on-failure
```

**예상 출력**:

```
[ RUN      ] MtlsContextTest.VerifyPeer_SelfSignedClientWithoutCA_Fails
[       OK ] MtlsContextTest.VerifyPeer_SelfSignedClientWithoutCA_Fails
```

**검증**: 서버는 `server_certs CA`만 신뢰 / 클라이언트는 `rogue CA` 서명 인증서 제시 → `SSL_accept` 실패 또는 `verify_peer() == false`.

---

## Phase 7-B/C — JWT 검증 (`test_auth_cases.cpp`, `JwtTest`)

### 시나리오 1 — 유효한 HS256 토큰 정상 검증

미래 `exp`를 가진 HS256 토큰을 동일 secret으로 `verify()`하면 Claims가 정확히 파싱되어야 한다.

```bash
ctest -R "JwtTest/HS256_ValidToken_ReturnsClaims" --output-on-failure
```

**예상 출력**:

```
[ RUN      ] JwtTest.HS256_ValidToken_ReturnsClaims
[       OK ] JwtTest.HS256_ValidToken_ReturnsClaims
```

**검증**: `claims.subject == "alice"`, `claims.issuer == "myserver"`, `claims.is_expired() == false`.

---

### 시나리오 2 — 유효한 RS256 토큰 정상 검증

RSA 개인키로 서명된 RS256 토큰을 공개키로 `verify()`하면 Claims가 파싱되어야 한다.

```bash
ctest -R "JwtTest/RS256_ValidToken_ReturnsClaims" --output-on-failure
```

**예상 출력**:

```
[ RUN      ] JwtTest.RS256_ValidToken_ReturnsClaims
[       OK ] JwtTest.RS256_ValidToken_ReturnsClaims
```

**검증**: `claims.subject == "bob"`, `claims.issuer == "auth-server"`, `claims.is_expired() == false`.

---

### 시나리오 3 — 만료된 토큰 → 예외

`exp < now`인 토큰은 `verify()`에서 예외를 던져야 한다.
만료 검증 없으면 탈취된 토큰이 영구적으로 유효해진다.

```bash
ctest -R "JwtTest/HS256_ExpiredToken_Throws" --output-on-failure
```

**예상 출력**:

```
[ RUN      ] JwtTest.HS256_ExpiredToken_Throws
[       OK ] JwtTest.HS256_ExpiredToken_Throws
```

**검증**: `past_exp = now - 1` → `verify()` → `std::runtime_error`.

---

### 시나리오 4 — 위조된 서명 → 예외

토큰 서명 마지막 바이트를 XOR 변조하면 `verify()`가 예외를 던져야 한다.

```bash
ctest -R "JwtTest/HS256_TamperedSignature_Throws" --output-on-failure
```

**예상 출력**:

```
[ RUN      ] JwtTest.HS256_TamperedSignature_Throws
[       OK ] JwtTest.HS256_TamperedSignature_Throws
```

**검증**: `sig_vec.back() ^= 0xFF` 변조 → `verify()` → `std::runtime_error`.

---

### 시나리오 5 — 알고리즘 불일치 → 예외

RS256 토큰을 HS256 검증기로 `verify()`하면 예외를 던져야 한다.
알고리즘 검증이 없으면 공격자가 헤더의 `alg`를 바꿔 서명 검증을 우회할 수 있다.

```bash
ctest -R "JwtTest/AlgorithmMismatch_HS256VerifierRS256Token_Throws" --output-on-failure
```

**예상 출력**:

```
[ RUN      ] JwtTest.AlgorithmMismatch_HS256VerifierRS256Token_Throws
[       OK ] JwtTest.AlgorithmMismatch_HS256VerifierRS256Token_Throws
```

**검증**: RS256 토큰 + HS256 검증기 → `verify()` → `std::runtime_error`.

---

### 시나리오 6 — 형식 오류 토큰 → 예외

`.`이 없거나 1개뿐인 문자열은 JWT 형식이 아니므로 즉시 예외를 던져야 한다.

```bash
ctest -R "JwtTest/MalformedToken_NoDots_Throws" --output-on-failure
ctest -R "JwtTest/MalformedToken_OneDot_Throws" --output-on-failure
```

**예상 출력**:

```
[ RUN      ] JwtTest.MalformedToken_NoDots_Throws
[       OK ] JwtTest.MalformedToken_NoDots_Throws
[ RUN      ] JwtTest.MalformedToken_OneDot_Throws
[       OK ] JwtTest.MalformedToken_OneDot_Throws
```

**검증**: `"notavalidtoken"`, `"header.payload"` → `std::runtime_error`.

---

### 시나리오 7 — exp 없는 토큰 → 만료 없이 통과

`exp` 클레임이 없는 토큰은 만료 없이 정상 통과해야 한다. `claims.expires == 0`, `is_expired() == false`.

```bash
ctest -R "JwtTest/HS256_NoExpClaim_PassesWithoutExpiry" --output-on-failure
```

**예상 출력**:

```
[ RUN      ] JwtTest.HS256_NoExpClaim_PassesWithoutExpiry
[       OK ] JwtTest.HS256_NoExpClaim_PassesWithoutExpiry
```

**검증**: payload에 `exp` 키 없음 → `claims.expires == 0`, `is_expired() == false`.

---

### 시나리오 8 — extract_bearer_token 파싱

`"Bearer <token>"` 형식에서 토큰만 추출하고, 접두사 없는 문자열은 빈 문자열을 반환해야 한다.

```bash
ctest -R "JwtTest/ExtractBearerToken" --output-on-failure
```

**예상 출력**:

```
[ RUN      ] JwtTest.ExtractBearerToken_ValidPrefix_ReturnsToken
[       OK ] JwtTest.ExtractBearerToken_ValidPrefix_ReturnsToken
[ RUN      ] JwtTest.ExtractBearerToken_NoPrefix_ReturnsEmpty
[       OK ] JwtTest.ExtractBearerToken_NoPrefix_ReturnsEmpty
```

**검증**: `"Bearer mytoken123"` → `"mytoken123"`, `"mytoken123"` → `""`.

---

### 시나리오 9 — 잘못된 PEM으로 RS256 생성 → 즉시 예외

유효하지 않은 PEM 문자열로 `create_rs256()`를 호출하면 객체 생성 시점에 즉시 예외를 던져야 한다.

```bash
ctest -R "JwtTest/RS256_InvalidPem_ThrowsOnCreate" --output-on-failure
```

**예상 출력**:

```
[ RUN      ] JwtTest.RS256_InvalidPem_ThrowsOnCreate
[       OK ] JwtTest.RS256_InvalidPem_ThrowsOnCreate
```

**검증**: `"not-a-pem"`, `""` → `std::runtime_error`.

---

## Phase 7-C — 접근 제어 정책 (`test_auth_cases.cpp`, `PolicyTest`)

### 시나리오 10 — 정확히 매칭되는 규칙 → 허용

subject / tunnel_id / target_ip / target_port 모두 일치하면 `is_allowed()` == true.

```bash
ctest -R "PolicyTest/ExactMatch_ReturnsTrue" --output-on-failure
```

**예상 출력**:

```
[ RUN      ] PolicyTest.ExactMatch_ReturnsTrue
[       OK ] PolicyTest.ExactMatch_ReturnsTrue
```

---

### 시나리오 11 — 필드 하나라도 불일치 → 차단

subject / tunnel_id / target_ip / target_port 중 하나라도 다르면 `is_allowed()` == false.

```bash
ctest -R "PolicyTest/SubjectMismatch_ReturnsFalse" --output-on-failure
ctest -R "PolicyTest/TunnelMismatch_ReturnsFalse" --output-on-failure
ctest -R "PolicyTest/IpMismatch_ReturnsFalse" --output-on-failure
ctest -R "PolicyTest/PortMismatch_ReturnsFalse" --output-on-failure
```

**예상 출력**: 4개 테스트 모두 `[OK]`.

**검증**: 각각 carol/agent-2/192.168.1.99/80 으로 교체 → false.

---

### 시나리오 12 — wildcard subject → 모든 사용자 허용

규칙의 subject가 `"*"`이면 임의의 사용자가 매칭되어야 한다.

```bash
ctest -R "PolicyTest/WildcardSubject_AnyUserAllowed" --output-on-failure
```

**예상 출력**:

```
[ RUN      ] PolicyTest.WildcardSubject_AnyUserAllowed
[       OK ] PolicyTest.WildcardSubject_AnyUserAllowed
```

**검증**: alice / bob / carol 모두 `is_allowed() == true`.

---

### 시나리오 13 — 완전 wildcard 규칙 → 모든 요청 허용

`{"*", "*", "*", 0}` 규칙이 존재하면 임의의 subject/tunnel/ip/port 조합이 모두 허용되어야 한다.

```bash
ctest -R "PolicyTest/FullWildcard_AllowsEverything" --output-on-failure
```

**예상 출력**:

```
[ RUN      ] PolicyTest.FullWildcard_AllowsEverything
[       OK ] PolicyTest.FullWildcard_AllowsEverything
```

**검증**: alice:agent-1:192.168.1.1:22, bob:agent-2:10.0.0.5:3306 등 모두 true.

---

### 시나리오 14 — first-match 순서 + default-deny

규칙 목록에서 첫 번째로 매칭되는 규칙이 적용된다.
매칭되는 규칙이 없으면 default-deny로 차단된다.

```bash
ctest -R "PolicyTest/FirstMatch" --output-on-failure
```

**예상 출력**:

```
[ RUN      ] PolicyTest.FirstMatch_EarlyRuleTakesPrecedence
[       OK ] PolicyTest.FirstMatch_EarlyRuleTakesPrecedence
[ RUN      ] PolicyTest.FirstMatch_DenyBeforeWildcard
[       OK ] PolicyTest.FirstMatch_DenyBeforeWildcard
```

**검증**: alice 전용 규칙만 있으면 bob → false (default-deny). wildcard가 뒤에 있으면 carol → true.

---

### 시나리오 15 — 빈 정책 → default-deny

규칙이 0개인 정책은 모든 요청을 차단해야 한다.
명시적 허용 없이 통과되면 Zero Trust 원칙 위반이다.

```bash
ctest -R "PolicyTest/EmptyPolicy_DefaultDeny" --output-on-failure
```

**예상 출력**:

```
[ RUN      ] PolicyTest.EmptyPolicy_DefaultDeny
[       OK ] PolicyTest.EmptyPolicy_DefaultDeny
```

**검증**: `rule_count() == 0`, `is_allowed("alice", "agent-1", "192.168.1.10", 22) == false`.

---

### 시나리오 16 — JSON 파싱 + 필수 필드 누락 → 예외

유효한 JSON에서는 규칙을 정상 파싱하고, `subject` 누락 / `rules` 키 없음 / JSON 아닌 문자열은 예외를 던져야 한다.

```bash
ctest -R "PolicyTest/LoadFromString" --output-on-failure
```

**예상 출력**:

```
[ RUN      ] PolicyTest.LoadFromString_ValidJson_ParsesCorrectly
[       OK ] PolicyTest.LoadFromString_ValidJson_ParsesCorrectly
[ RUN      ] PolicyTest.LoadFromString_MissingSubject_Throws
[       OK ] PolicyTest.LoadFromString_MissingSubject_Throws
[ RUN      ] PolicyTest.LoadFromString_MissingRulesKey_Throws
[       OK ] PolicyTest.LoadFromString_MissingRulesKey_Throws
[ RUN      ] PolicyTest.LoadFromString_InvalidJson_Throws
[       OK ] PolicyTest.LoadFromString_InvalidJson_Throws
```

**검증**: 정상 JSON → `rule_count() == 2`, 나머지 → `std::runtime_error`.

---

### 시나리오 17 — load_from_file() 파일 파싱 + 없는 파일 → 예외

존재하지 않는 경로는 예외, 유효한 파일은 규칙을 정상 로드해야 한다.

```bash
ctest -R "PolicyTest/LoadFromFile" --output-on-failure
```

**예상 출력**:

```
[ RUN      ] PolicyTest.LoadFromFile_NonExistentPath_Throws
[       OK ] PolicyTest.LoadFromFile_NonExistentPath_Throws
[ RUN      ] PolicyTest.LoadFromFile_ValidFile_ParsesCorrectly
[       OK ] PolicyTest.LoadFromFile_ValidFile_ParsesCorrectly
```

**검증**: `/tmp/no_such_file_xyz.json` → `std::runtime_error`, 임시 파일 → `rule_count() == 1`.

---

## 전체 Phase 7 테스트 한 번에

```bash
cd ~/tunnel-proxy/build
ctest -L phase7 --output-on-failure
```

**예상 출력**:

```
[==========] Running 27 tests from 3 test suites.
...
[==========] 27 tests passed.

100% tests passed, 0 tests failed out of 27
Total Test time (real) = X.XXs
```
