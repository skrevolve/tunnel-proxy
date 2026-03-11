# Phase 4 테스트 가이드

Phase 3까지는 "데이터가 전달되는가"를 봤다.
Phase 4는 **TLS 암호화 채널이 올바르게 수립되는가**를 검증한다.
잘못된 인증서를 거부하는지, 평문 연결을 차단하는지가 핵심이다.

---

## 환경 설정

인증서 생성:

```bash
cd ~/tunnel-proxy
bash scripts/gen_cert.sh
```

생성 결과:

```
certs/server.crt  — 서버 인증서 (공개)
certs/server.key  — 서버 개인키 (비밀)
```

---

## openssl 이란?

TLS 소켓을 터미널에서 직접 열고 닫을 수 있는 암호화 유틸리티.
`nc`가 평문 TCP를 담당한다면 `openssl s_client / s_server`는 TLS 소켓을 담당한다.

```
# TLS 서버 모드
openssl s_server -cert certs/server.crt -key certs/server.key -port 8443 -quiet

# TLS 클라이언트 모드 (-CAfile: 서버 인증서 검증에 쓸 CA)
openssl s_client -connect localhost:8443 -CAfile certs/server.crt -quiet
```

시나리오에서 각 역할:

```
[openssl s_client]  ←→  [TlsProxy 8443→8000]  ←→  [nc -l -p 8000]
   TLS 클라이언트           TLS 프록시                  평문 타겟
```

---

## ctest 라벨 안내

`gtest_discover_tests`는 테스트를 `TestSuite.TestName` 형식으로 등록한다.
`ctest -R test_phase4`처럼 실행 파일 이름으로는 필터링되지 않는다.
Phase 단위로 실행하려면 `-L` (라벨) 옵션을 사용한다.

```bash
ctest -L phase4 --output-on-failure      # Phase 4 전체
ctest -R "TlsCtxTest" --output-on-failure       # 특정 suite만
ctest -R "TlsHandshakeTest.ValidCertSucceeds" --output-on-failure  # 특정 케이스만
```

---

## 시나리오 1 — TLS 컨텍스트 초기화

유효한 인증서/키 파일로 TlsProxy가 정상적으로 초기화되는지 확인한다.
잘못된 경로나 쌍이 맞지 않는 인증서/키를 주면 예외가 발생해야 한다.

```bash
cd ~/tunnel-proxy/build
ctest -R "TlsCtxTest" --output-on-failure
```

**예상 출력**:

```
[ RUN      ] TlsCtxTest.ValidCertLoads
[       OK ] TlsCtxTest.ValidCertLoads
[ RUN      ] TlsCtxTest.MissingCertFileThrows
[       OK ] TlsCtxTest.MissingCertFileThrows
[ RUN      ] TlsCtxTest.MissingKeyFileThrows
[       OK ] TlsCtxTest.MissingKeyFileThrows
[ RUN      ] TlsCtxTest.MismatchedKeyThrows
[       OK ] TlsCtxTest.MismatchedKeyThrows
```

**검증**: 4개 모두 통과하면 `SSL_CTX_use_certificate_file` / `SSL_CTX_use_PrivateKey_file` / `SSL_CTX_check_private_key` 흐름 정상.

---

## 시나리오 2 — 유효한 인증서로 핸드셰이크 성공

유효한 인증서를 가진 서버에 클라이언트가 TLS 핸드셰이크를 맺는지 확인한다.

```bash
ctest -R "TlsHandshakeTest.ValidCertSucceeds" --output-on-failure
```

**예상 출력**:

```
[ RUN      ] TlsHandshakeTest.ValidCertSucceeds
[       OK ] TlsHandshakeTest.ValidCertSucceeds
```

**검증**: `SSL_accept` + `SSL_connect`가 모두 반환 1이면 핸드셰이크 성공.

---

## 시나리오 3 — 만료된 인증서 거부

어제 만료된 인증서를 서버가 사용할 때 클라이언트가 거부하는지 확인한다.
`SSL_VERIFY_PEER`가 활성화된 클라이언트는 만료 인증서를 수락해서는 안 된다.

```bash
ctest -R "TlsHandshakeTest.ExpiredCertRejected" --output-on-failure
```

**예상 출력**:

```
[ RUN      ] TlsHandshakeTest.ExpiredCertRejected
[       OK ] TlsHandshakeTest.ExpiredCertRejected
```

**검증**: `SSL_connect`가 실패(-1)를 반환하고 `SSL_get_error`가 `SSL_ERROR_SSL`이면 인증서 검증 거부 정상.

---

## 시나리오 4 — 평문 TCP 연결 차단

TLS 없이 평문으로 연결하면 차단되는지 확인한다.
TLS 서버에 일반 `nc`로 붙으면 핸드셰이크 단계에서 프로토콜 오류가 발생해야 한다.

```bash
ctest -R "TlsHandshakeTest.PlainTextRejected" --output-on-failure
```

**예상 출력**:

```
[ RUN      ] TlsHandshakeTest.PlainTextRejected
[       OK ] TlsHandshakeTest.PlainTextRejected
```

**검증**: 평문 데이터를 받은 `SSL_accept`가 실패하면 차단 정상.

---

## 전체 Phase 4 테스트 한 번에

```bash
cd ~/tunnel-proxy/build
ctest -L phase4 --output-on-failure
```

**예상 출력**:

```
100% tests passed, 0 tests failed out of 7
Total Test time (real) = X.XXs
```
