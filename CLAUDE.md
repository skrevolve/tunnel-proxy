# CLAUDE.md — tunnel-proxy

## 프로젝트 목표

VPN/원격접속 기술을 학습하고 직접 구현하는 프로젝트.

구현 목표 (단계별):
1. **리버스 터널** — 클라이언트가 NAT 뒤에 있어도 서버에서 연결 가능하게
2. **Zero Trust 인증** — 네트워크 신뢰 없이 매 요청마다 인증 (mTLS/JWT)
3. **Guacamole 프로토콜** — 서버에서 직접 렌더링해서 브라우저로 전달 (RDP/VNC/SSH 웹화)
4. **HTTP 웹 프록시** — Guacamole 커스텀 `web` 프로토콜로 HTTP/HTTPS 페이지를 게이트웨이에서 가져와 브라우저에 전달. 기본은 `GuacHttpClient`(libcurl 직접 요청), 선택적으로 `GuacWebClient`(Chromium headless CDP) 전환 가능

최종 결과물: Cloudflare Tunnel + Apache Guacamole + HTTP 웹 프록시를 C++로 직접 만든 것

---

## 현재 개발 단계

**Phase 14 진행 중** — HTTP 웹 프록시 (GuacHttpClient)

- [x] Phase 1 — 기본 TCP 프록시 (멀티스레드) **완료**
- [x] Phase 2 — epoll 비동기 I/O **완료**
- [x] Phase 3 — Zero-copy 최적화 (splice/sendfile) **완료**
- [x] Phase 4 — TLS 암호화 (OpenSSL) **완료**
- [x] Phase 5 — UDP 지원 **완료**
- [x] Phase 6 — 리버스 터널 프로토콜 **완료**
- [x] Phase 7 — Zero Trust 인증 (mTLS / JWT) **완료**
- [x] Phase 8 — Guacamole 프로토콜 연동 **완료**
- [x] Phase 8-F — React 프론트엔드 (SSH 터미널 렌더링 + WebSocket 클라이언트) **완료**
- [ ] ~~Phase 9 — 컨트롤 플레인~~ **보류** (SaaS 구조 전제, 현재 목표와 맞지 않음)
- [ ] ~~Phase 10 — REST API~~ **보류** (동일 이유)
- [x] Phase 11 — 재연결 및 안정성 **완료**
- [ ] Phase 12 — 배포 **보류** (Phase 14 완료 후 진행)
- [x] Phase 13 — Remote Browser Isolation (GuacWebClient + Chromium CDP) **완료**
- [ ] Phase 14 — HTTP 웹 프록시 (GuacHttpClient + 터널 라우팅 통합) **다음**

---

## 세션 작업 단위 (토큰 절약)

**한 세션에 하나의 작업 단위만 처리.**
세션 시작 시 현재 세션 번호를 전달받으면 해당 작업만 완료하고 PR까지 마무리.
다음 세션 범위는 TODO 주석만 남기고 절대 건드리지 않음.

**세션 종료 기준: PR 생성 완료 = 세션 종료.**
PR을 올린 뒤 다음 Phase 작업을 이어서 시작하지 않는다.
이유: 세션이 길어질수록 컨텍스트 요약이 누적되어 다음 세션의 초기 토큰 비용이 폭증한다.

### Phase 1 — 기본 TCP 프록시 (멀티스레드)

| 세션 | 브랜치 | 작업 | 왜 필요한가 |
|------|--------|------|-------------|
| 1-A | `feat/cmake-json` | CMakeLists.txt에 nlohmann/json FetchContent 추가 | config.json 파싱에 필요. apt 대신 FetchContent를 쓰는 이유는 WSL/Ubuntu/Docker 등 환경이 달라져도 cmake 한 번으로 동일하게 동작하게 하기 위함 |
| 1-B | `feat/config-parse` | Config::load_from_file() JSON 실제 파싱 구현 | 현재 하드코딩된 기본값만 반환. config.json의 port/ip/mode 등을 실제로 읽어야 프록시가 설정대로 동작 |
| 1-C | `feat/socket-listen` | create_listening_socket() + run() accept 루프 | 소켓을 열고 클라이언트 연결을 받는 핵심. 이게 없으면 프록시가 아무것도 안 함 |
| 1-D | `feat/forward-data` | connect_to_target() + handle_connection() + forward_data() | 클라이언트 ↔ 타겟 서버 간 양방향 데이터 전달. Phase 1의 핵심 기능 |
| 1-E | `refactor/cleanup` | proxy.h 삭제, running_ → atomic<bool>, 테스트 보강 | proxy.h는 BasicProxy와 중복. running_은 멀티스레드 환경에서 data race 방지를 위해 atomic 필요 |

### Phase 2 — epoll 비동기 I/O

| 세션 | 브랜치 | 작업 | 왜 필요한가 |
|------|--------|------|-------------|
| 2-A | `feat/epoll-proxy` | EpollProxy 클래스 skeleton + non-blocking 소켓 | Phase 1의 멀티스레드 방식은 연결마다 스레드를 생성해서 수백 개 동시접속 시 메모리/컨텍스트 스위칭 비용이 큼. epoll은 단일 스레드로 수만 개 연결을 처리 가능 |
| 2-B | `feat/epoll-loop` | epoll_wait 이벤트 루프 구현 | 이벤트 기반으로 I/O 준비된 fd만 처리. 리버스 터널에서 대량 연결을 유지하는 데 필수 |
| 2-C | `feat/epoll-forward` | edge-triggered 양방향 데이터 포워딩 | ET 모드는 LT보다 시스템 콜 횟수가 적어 성능 우위. Zero-copy 최적화(Phase 3)의 전제 조건 |

### Phase 3 — Zero-copy 최적화 (splice/sendfile)

| 세션 | 브랜치 | 작업 | 왜 필요한가 |
|------|--------|------|-------------|
| 3-A | `perf/splice-forward` | splice() 기반 forward_data 구현 | read→write 방식은 커널→유저→커널 메모리 복사 2회 발생. splice는 커널 내부에서 파이프를 통해 복사 없이 전달해 CPU 사용량 절감 |
| 3-B | `perf/pipe-buffer` | pipe 버퍼 풀 관리 + SPLICE_F_MOVE 최적화 | splice는 중간에 pipe fd가 필요. 연결마다 pipe를 생성/소멸하면 오히려 오버헤드. 풀로 재사용해야 실질적 성능 이득 |
| 3-C | `test/zero-copy-bench` | EpollProxy 대비 처리량 측정 + 테스트 | 최적화 효과를 수치로 확인. 대용량 전송 시 CPU 사용률 비교 |

### Phase 4 — TLS 암호화 (OpenSSL)

| 세션 | 브랜치 | 작업 | 왜 필요한가 |
|------|--------|------|-------------|
| 4-A | `feat/tls-context` | SSL_CTX 초기화 + 인증서/키 로딩 + gen_cert.sh 정비 | TLS의 기반. 인증서 없이는 암호화도 신원 확인도 불가 |
| 4-B | `feat/tls-handshake` | SSL_accept / SSL_connect 핸드셰이크 구현 | 실제 암호화 채널 수립. 핸드셰이크 실패 시 연결 차단 |
| 4-C | `feat/tls-forward` | SSL_read / SSL_write 기반 포워딩 | 일반 read/write를 SSL 버전으로 교체. 논블로킹 + WANT_READ/WANT_WRITE 처리 필요 |
| 4-D | `test/tls-verify` | 인증서 검증 테스트 (유효/만료/자체서명) | TLS가 실제로 잘못된 인증서를 거부하는지 확인 |

### Phase 5 — UDP 지원

| 세션 | 브랜치 | 작업 | 왜 필요한가 |
|------|--------|------|-------------|
| 5-A | `feat/udp-proxy` | UdpProxy 헤더 설계 + recvfrom/sendto 기반 구현 | VPN 트래픽의 상당수가 UDP. TCP 전용이면 게임/영상통화 등 지연 민감한 트래픽 처리 불가 |
| 5-B | `feat/udp-session` | UDP 세션 테이블 (클라이언트 addr → 타겟 소켓 매핑) | UDP는 연결 개념이 없어 패킷마다 출처를 확인해야 함. 세션 테이블로 클라이언트별 상태 유지 |
| 5-C | `test/udp-forward` | UDP 양방향 포워딩 테스트 | nc -u로 UDP 패킷 전달 검증 |

### Phase 6 — 리버스 터널 프로토콜

| 세션 | 브랜치 | 작업 | 왜 필요한가 |
|------|--------|------|-------------|
| 6-A | `feat/tunnel-protocol` | 터널 프로토콜 헤더 설계 (magic / type / session_id / length / payload) | 에이전트↔서버 간 메시지를 구분할 바이너리 프로토콜 정의. 없으면 여러 세션을 하나의 TCP 연결에서 구분 불가 |
| 6-B | `feat/tunnel-agent` | TunnelAgent — 서버로 역방향 연결 유지 + heartbeat 송신 | 엔진의 핵심. NAT 뒤 클라이언트가 서버에 먼저 연결해두는 구조. 이게 없으면 서버에서 클라이언트에 연결할 방법이 없음 |
| 6-C | `feat/tunnel-server` | TunnelServer — 에이전트 연결 수신 + 세션 ID 발급 + 세션 맵 관리 | 서버 측에서 어떤 에이전트가 연결되어 있는지 추적. 외부 요청이 들어오면 올바른 에이전트 터널로 전달 |
| 6-D | `feat/tunnel-forward` | 외부 클라이언트 요청 → 터널 역방향 포워딩 | 실제 데이터 흐름 완성. 외부→서버→터널→에이전트→내부서버 경로 구현 |

### Phase 7 — Zero Trust 인증 (mTLS / JWT)

| 세션 | 브랜치 | 작업 | 왜 필요한가 |
|------|--------|------|-------------|
| 7-A | `feat/mtls-verify` | mTLS 클라이언트 인증서 검증 + CA 체인 확인 | 네트워크 위치와 무관하게 인증서로 신원 확인. VPN처럼 네트워크 자체를 신뢰하지 않는 Zero Trust의 핵심 |
| 7-B | `feat/jwt-parse` | JWT 파싱 + 서명 검증 (HS256/RS256) | HTTP 헤더의 Bearer 토큰으로 사용자/서비스 인증. mTLS가 장치 인증이라면 JWT는 사용자 인증 |
| 7-C | `feat/access-policy` | 접근 제어 정책 (터널별 / 사용자별 허용 규칙) | 인증만으로는 부족. 인증된 사용자가 어떤 터널/리소스에 접근 가능한지 제어 필요 |
| 7-D | `test/auth-cases` | 유효/만료/위조 토큰 + 미인증 접근 차단 테스트 | 보안 로직은 정상 케이스보다 실패 케이스가 더 중요 |

### Phase 8 — Guacamole 프로토콜 연동

| 세션 | 브랜치 | 작업 | 왜 필요한가 |
|------|--------|------|-------------|
| 8-A | `feat/guac-parser` | Guacamole instruction 파서 (길이 접두사 텍스트 프로토콜) | 브라우저↔게이트웨이 간 통신 포맷. 이게 없으면 브라우저에서 RDP/VNC를 제어할 수 없음 |
| 8-B | `feat/guac-rdp` | RDP 연결 + 화면 스트리밍 (libfreerdp FetchContent) | 윈도우 원격 데스크톱. 기업 환경에서 가장 많이 쓰이는 원격 프로토콜 |
| 8-C | `feat/guac-ssh` | SSH 연결 + 터미널 스트리밍 (libssh2 FetchContent) | 서버 관리의 표준. 브라우저에서 바로 SSH 터미널 접근 |
| 8-D | `feat/guac-vnc` | VNC 연결 + 화면 스트리밍 | Linux 데스크톱 원격 접속. RDP를 지원하지 않는 환경 대응 |
| 8-E | `feat/guac-websocket` | 브라우저 WebSocket 연결 + Guacamole 스트림 연결 | 브라우저는 TCP를 직접 못 씀. WebSocket으로 감싸서 Guacamole 프로토콜 전달 |

### Phase 9 — 컨트롤 플레인 (**보류**)

> **보류 이유**: 이 프로젝트는 개인/소규모 팀이 자기 서버에 직접 설치해서 쓰는 구조를 목표로 한다.
> Phase 9~10은 여러 고객을 한 서버로 서비스하는 SaaS 구조를 전제로 설계된 것으로,
> 현재 목표와 맞지 않아 보류. "다른 사람에게도 배포하는 서비스"로 목표가 바뀌면 재개.

| 세션 | 브랜치 | 작업 | 왜 필요한가 |
|------|--------|------|-------------|
| 9-A | `feat/tunnel-registry` | 터널 레지스트리 — 에이전트 등록/해제/상태 추적 | Phase 6까지는 에이전트가 하나라는 가정. 여러 에이전트를 동시에 관리하려면 중앙 레지스트리 필요 |
| 9-B | `feat/session-router` | 세션 라우터 — 외부 요청을 올바른 터널로 매핑 | 에이전트가 여럿일 때 요청이 어느 터널로 가야 하는지 결정하는 라우팅 로직 |
| 9-C | `feat/multi-tenant` | 멀티 테넌트 — 사용자별 터널 격리 | 같은 서버에서 여러 사용자의 터널이 서로 간섭하지 않도록 격리. 실제 서비스 운영의 전제 조건 |

### Phase 10 — REST API (**보류**)

> **보류 이유**: Phase 9와 동일. config.json 수정 + 재시작으로 충분한 운영 구조에서는 불필요.
> 관리 UI/API가 필요한 시점에 재개.

| 세션 | 브랜치 | 작업 | 왜 필요한가 |
|------|--------|------|-------------|
| 10-A | `feat/http-server` | 경량 HTTP 서버 (cpp-httplib FetchContent) | 터널 상태를 외부에서 조회/제어하려면 API 엔드포인트 필요. 관리 UI의 백엔드 역할 |
| 10-B | `feat/api-tunnels` | GET /tunnels, GET /tunnels/:id 엔드포인트 | 현재 연결된 에이전트 목록과 상태를 조회. 운영 중 모니터링의 기본 |
| 10-C | `feat/api-manage` | POST /tunnels, DELETE /tunnels/:id 엔드포인트 | 터널 생성/삭제를 API로 제어. 자동화 배포/스크립트에서 활용 |
| 10-D | `feat/api-auth` | API 인증 미들웨어 (API 키 검증) | 관리 API가 인증 없이 열려있으면 누구나 터널을 제어 가능. 최소한 API 키로 보호 필요 |

### Phase 11 — 재연결 및 안정성

| 세션 | 브랜치 | 작업 | 왜 필요한가 |
|------|--------|------|-------------|
| 11-A | `feat/heartbeat` | Heartbeat ping/pong + 타임아웃 감지 | 네트워크 단절 시 TCP는 한참 후에야 연결 종료를 감지. Heartbeat로 빠르게 감지하고 재연결 트리거 |
| 11-B | `feat/reconnect` | 지수 백오프 자동 재연결 (1s → 2s → 4s → max 60s) | 서버 재시작/네트워크 불안정 시 에이전트가 자동 복구. 운영 환경에서 필수 |
| 11-C | `feat/metrics` | 연결 수 / 전송량 / 오류율 메트릭 수집 + 로그 출력 | 문제 발생 시 원인 추적. 장기 운영 시 성능 트렌드 파악 |

### Phase 12 — 배포

| 세션 | 브랜치 | 작업 | 왜 필요한가 |
|------|--------|------|-------------|
| 12-A | `build/dockerfile` | Dockerfile (서버용 / 에이전트용 분리) | 환경에 무관하게 동일하게 배포. 서버와 에이전트는 역할이 달라 이미지를 분리 |
| 12-B | `build/systemd` | systemd 서비스 파일 (자동 시작 / 재시작 정책) | 리눅스 서버에서 프로세스를 데몬으로 관리. 서버 재부팅 후 자동 복구 |
| 12-C | `build/compose` | docker-compose (서버 + 게이트웨이 + 설정 볼륨) | 서버/게이트웨이를 한 번에 올리는 로컬 개발 및 배포 환경 구성 |

### Phase 13 — Remote Browser Isolation (GuacWebClient + Chromium CDP) **완료**

Guacamole 커스텀 `web` 프로토콜로 Chromium headless CDP를 제어해 스크린샷을 캡처하고 blob 스트리밍으로 브라우저 canvas에 전달.
Chromium은 cmake configure 시 `scripts/setup_chromium.sh`가 자동으로 탐지/설치한다.

| 세션 | 브랜치 | 작업 |
|------|--------|------|
| 13-A | `feat/guac-web-cdp` | Chrome fork/exec + CDP WebSocket + 페이지 로드 |
| 13-B | `feat/guac-web-stream` | captureScreenshot → JPEG blob 스트리밍 + 라우팅 |
| 13-C | `feat/guac-web-input` | 마우스/키보드 → CDP Input 이벤트 주입 |
| 13-D | `feat/guac-web-delta` | 전 프레임 비교 delta 압축 전송 |
| 13-E | `test/guac-web-e2e` | E2E 테스트 |

### Phase 14 — HTTP 웹 프록시 (GuacHttpClient + 터널 라우팅 통합)

Phase 13의 Chromium 방식은 프로세스 오버헤드가 크고 터널 라우팅이 빠져있다.
Phase 14는 두 가지 문제를 해결한다:

1. **GuacHttpClient** — libcurl 기반 HTTP GET. `response,<status>,<content-type>,<base64-body>;` Guacamole instruction으로 반환. 기본 `web` 구현.
2. **터널 라우팅 통합** — GuacWebSocketGateway가 TunnelServer를 참조해, NAT 뒤 에이전트를 거쳐 내부 웹 서비스에 도달할 수 있도록 연결.
3. **web_renderer 설정** — config.json의 `"web_renderer": "http"` (기본) 또는 `"chromium"` 으로 구현체 선택.

Guacamole `web` 프로토콜 흐름:
```
브라우저 → connect,web,https://내부서버/path;
         → GuacWebSocketGateway
              ├─ web_renderer=http   → GuacHttpClient → (터널 or 직접) → HTTP GET
              └─ web_renderer=chromium → GuacWebClient → Chromium CDP
         → response,200,text/html,<base64>;
브라우저 → iframe src=data:... 렌더링
```

새 Guacamole instruction:
```
response,<status_code>,<content-type>,<base64-encoded-body>;
```

| 세션 | 브랜치 | 작업 | 왜 필요한가 |
|------|--------|------|-------------|
| 14-A | `feat/guac-http` | GuacHttpClient (libcurl) + `response` instruction + config `web_renderer` + GuacWebSocketGateway 라우팅 | HTTP 직접 구현. Chromium 없이 웹 페이지 접근 가능. libcurl은 시스템 의존성(find_package) |
| 14-B | `feat/guac-tunnel-route` | GuacWebSocketGateway ↔ TunnelServer 연결 — agent_id 기반 터널 경유 HTTP | Guacamole와 리버스 터널이 분리되어 있어 NAT 뒤 웹 서비스에 접근 불가. 이 작업으로 연결 |
| 14-C | `feat/guac-http-frontend` | 프론트엔드 `response` instruction 처리 — iframe 또는 새 탭 렌더링 | 브라우저가 base64 HTML을 받아 표시하는 UI 완성 |

---

## Git 워크플로우 (커밋별 PR)

커밋마다 feature 브랜치 → PR 생성까지 Claude가 담당.
**PR 머지는 반드시 사람이 직접 한다.**

### 1. 테스트 시나리오 정의 (Claude → 사람 확인)

**테스트가 포함된 세션 = Phase 마지막 세션에서만 해당.**

Claude가 먼저 시나리오 목록을 작성해 제안한다.
사람이 "이게 내가 원하는 검증인가" 확인 후 납득되면 구현 진행. 아니면 수정.

**Claude 필수 준수 사항:**
- Phase 마지막 세션 시작 시 코드를 한 줄도 작성하기 전에 시나리오 목록을 먼저 제안한다.
- 사람의 확인(ㅇㅇ / 네 / 수정 요청) 없이 테스트 코드 작성을 시작하지 않는다.
- 시나리오 → 사람 확인 → 테스트 코드 작성 → 사람 확인 → 구현. 이 순서를 절대 건너뛰지 않는다.

### 2. 브랜치 생성 (Claude)

항상 master를 최신화한 뒤 브랜치를 딴다.
master에 ci.yml 등 공통 파일이 있으므로, 최신 master 기준으로 브랜치해야
push 즉시 CI가 트리거된다.

```bash
git checkout master && git pull origin master
git checkout -b <type>/<scope>
# 예: feat/tls-context, fix/accept-loop
```

### 3. 테스트 작성 → 구현 → 빌드 확인 (Claude)

로컬 빌드로 컴파일 에러만 확인한다. ctest는 실행하지 않는다 — CI가 대신한다.

```bash
cd build && cmake -DCMAKE_BUILD_TYPE=Debug .. && make -j$(nproc)
# 빌드 성공 확인 후 push. ctest는 CI에서 자동 실행됨.
```

### 4. 커밋 + push + CI 확인 + PR 생성 (Claude)

```bash
git add <파일>
git commit -m "<type>(<scope>): <description>"
git push origin <브랜치명>

# CI 결과 확인 (push 후 ~1분 대기)
gh run list --branch <브랜치명> --limit 1
gh run view <run-id>   # pass/fail 확인

# CI 통과 후 PR 생성
gh pr create --title "<커밋 제목>" --body "..." --base master
# 여기서 Claude 작업 종료. 머지는 하지 않는다.
```

커밋 타입: `feat` / `fix` / `perf` / `refactor` / `test` / `docs` / `build`

### 5. PR 검토 + 머지 (사람)

GitHub에서 PR을 열고 아래 항목을 직접 확인한다.

```
[ ] 핵심 구현 결정 표 — 왜 이렇게 짰는가 납득되는가
[ ] 테스트 코드 — 내가 요청한 시나리오가 실제로 검증되고 있는가
[ ] 빌드 및 테스트 결과 — 모두 통과했는가
[ ] 직접 동작 확인 — nc/curl 등으로 실제로 동작하는가
```

납득되면 GitHub에서 직접 **Squash and merge**.
의문이 있으면 PR에 댓글로 질문 → Claude 답변 → 재확인.

PR 본문 필수 항목 — 처음 보는 사람도 코드를 읽지 않고 PR만으로 완전히 이해할 수 있을 정도로 상세하게 작성.

```
## 작업 배경
이전 단계의 어떤 구조적 한계 때문에 이 작업이 필요해졌는가.
추상적인 표현 금지. 숫자, 시나리오, 구체적인 상황으로 설명.

나쁜 예) "멀티스레드 방식은 비효율적이다"
좋은 예) "BasicProxy는 연결마다 스레드를 생성한다. 스레드 1개당 기본
         스택 8MB. 동시접속 1000개 = 메모리 8GB 소비. 대부분의 시간은
         I/O 대기 상태인데도 스레드가 CPU를 점유하고 있다."

## 핵심 개념
이 작업에서 처음 등장하는 기술/시스템 콜/자료구조를 한 줄씩 설명.
이 섹션만 읽어도 코드의 맥락이 잡혀야 한다.
공식 문서 링크 대신 "한 줄 본질 요약"을 쓸 것.

예)
- epoll: 관심 fd를 등록하면 커널이 I/O 준비된 fd만 골라 알려줌.
         select/poll은 등록된 fd 전체를 매번 순회(O(n))하지만
         epoll은 준비된 것만 반환(O(1)).
- EPOLLET: edge-triggered. 상태 변화 시 딱 1번만 이벤트 발생.
           반드시 EAGAIN이 날 때까지 데이터를 전부 읽어야 함.
- EAGAIN: "지금 읽을 데이터 없음". 논블로킹 소켓에서 이게 오면
          이벤트 루프로 돌아가 다음 이벤트를 기다려야 한다.

## 변경 사항
파일별로 무엇을 추가/수정했는가. 함수 단위로 기록.

- src/epoll_proxy.cpp
  - create_listening_socket(): 기존과 동일하나 O_NONBLOCK 추가
  - add_to_epoll(): EPOLLIN | EPOLLET 플래그로 fd 등록
  - run(): epoll_wait 루프 — 이벤트 배열 순회하며 accept/forward 분기
  - forward_data(): EAGAIN 루프로 버퍼 소진까지 읽기

## 핵심 구현 결정
코드에서 "왜 이렇게 썼는가"가 궁금할 만한 선택들.
선택한 이유와 다른 방법을 안 쓴 이유를 함께 기록.

| 코드 / 선택 | 이유 | 다른 방법을 안 쓴 이유 |
|-------------|------|------------------------|
| EPOLLET 사용 | 이벤트 중복 발생 없이 1번만 통지 → 불필요한 wake-up 제거 | EPOLILT(level-triggered)는 데이터가 남아있는 한 계속 이벤트 발생, 처리 완료 전 같은 fd로 반복 진입 |
| EAGAIN까지 루프 | ET 모드는 새 데이터 도착 시에만 다음 이벤트가 옴 | 한 번만 read하면 버퍼에 데이터가 남아도 이벤트가 안 와 무한 대기 |
| epoll_create1(EPOLL_CLOEXEC) | fork/exec 시 자식 프로세스에 fd 누출 방지 | epoll_create(0) 쓰면 나중에 exec 계열 호출 시 자식이 epoll fd를 물고 있어 GC 안 됨 |
| peer_fd 저장 (ConnState) | 한쪽 fd가 닫힐 때 반대쪽도 즉시 정리 | 저장 안 하면 client fd 닫혔을 때 연결된 target fd를 찾을 방법이 없어 좀비 fd 누적 |

## 빌드 및 테스트 결과
실제 터미널 출력을 그대로 붙여넣기.

\`\`\`
[100%] Built target proxy
[100%] Built target test_phase2

100% tests passed, 0 tests failed out of N
Total Test time (real) = 0.XXs
\`\`\`

## 이 작업으로 가능해진 것 / 다음 단계
이전과 비교해서 무엇이 달라졌는가.
다음 세션에서 이어받을 부분.

예)
- 이전: 연결 1000개 = 스레드 1000개 = 메모리 8GB
  이후: 연결 1000개 = 스레드 1개 + epoll 이벤트 루프
- 다음 세션(2-C): ET 모드 기반 양방향 포워딩 + EAGAIN 처리 구현
```

---

## 의존성 규칙

**apt, brew, yum 등 시스템 패키지 매니저 절대 사용 금지.**
이유: WSL/Ubuntu/Docker/서버 등 환경마다 패키지 이름과 버전이 달라 의존성이 깨짐.
모든 외부 라이브러리는 CMakeLists.txt의 FetchContent로 관리.

시스템 의존성 (find_package 또는 경로 탐지):
- CMake 3.14+
- GCC 11+ (C++17)
- OpenSSL 3.x
- pthread
- Chromium (Phase 13 전용) — cmake configure 시 `scripts/setup_chromium.sh` 자동 탐지/설치 (`~/.local/share/tunnel-proxy/chromium/`)

FetchContent로 관리하는 라이브러리:
- nlohmann/json v3.11.3
- Google Test v1.14.0

FetchContent 추가 방법:
```cmake
FetchContent_Declare(
    <이름>
    URL https://github.com/<경로>/archive/refs/tags/<버전>.tar.gz
)
FetchContent_MakeAvailable(<이름>)
```

---

## 빌드 규칙

```bash
cd build && cmake -DCMAKE_BUILD_TYPE=Debug .. && make -j$(nproc)
```

빌드 실패 시 에러 읽고 수정 → 재빌드 성공 확인 후 진행.
빌드 실패 상태로 커밋 절대 금지.

---

## 테스트 규칙

```bash
cd build && ctest --output-on-failure
```

테스트 실패 상태로 커밋 금지.

동작 확인:
```bash
# 서버 실행
./proxy                  # :9900 TunnelServer + :8765 GuacWebSocketGateway

# 에이전트 실행 (별도 머신 또는 터미널)
./agent -c agent.json    # proxy:9900으로 역방향 연결

# 브라우저 접속
cd frontend && npm run dev   # http://localhost:5173 → ws://서버:8765
```

---

## 코드 규칙

**언어/표준**: C++17, GCC 11+

**네이밍**:
- 클래스: PascalCase (`BasicProxy`, `EpollProxy`)
- 함수/변수: snake_case (`handle_connection`)
- 멤버 변수: snake_case + `_` 접미사 (`listen_fd_`)
- 상수: UPPER_SNAKE_CASE (`MAX_BUFFER_SIZE`)

**메모리**:
- `new`/`delete` 직접 사용 금지
- `std::unique_ptr`, `std::shared_ptr` 사용
- RAII 패턴 준수 (소켓 fd도 RAII 래퍼로 관리)

**네트워크**:
- 모든 시스템 콜 리턴값 반드시 체크 (`errno` 확인)
- 소켓 close 누락 금지
- Phase 2부터 non-blocking + edge-triggered epoll 기본

**에러 처리**:
```cpp
if (fd < 0) {
    throw std::runtime_error("socket: " + std::string(strerror(errno)));
}
```

**헤더 가드**: `#pragma once`
**네임스페이스**: 모든 코드는 `namespace proxy {}` 안에

---

## 디렉토리 구조

```
tunnel-proxy/
├── include/
│   ├── core/
│   │   ├── basic_proxy.h        # Phase 1 완료
│   │   ├── epoll_proxy.h        # Phase 2 진행 중
│   │   ├── tls_proxy.h          # Phase 4 예정
│   │   ├── tunnel_agent.h       # Phase 6 예정
│   │   ├── tunnel_server.h      # Phase 6 예정
│   │   └── control_plane.h      # Phase 9 예정
│   └── utils/
│       ├── config.h
│       └── logger.h
├── src/
│   ├── main.cpp
│   ├── basic_proxy.cpp
│   ├── epoll_proxy.cpp
│   └── utils/
│       ├── config.cpp
│       └── logger.cpp
├── tests/
│   ├── CMakeLists.txt
│   ├── phase1/
│   │   ├── test_config.cpp
│   │   └── test_logger.cpp
│   └── phase2/                  # Phase 2 완료 후 활성화
├── scripts/
│   ├── build.sh
│   ├── test.sh
│   └── gen_cert.sh              # Phase 4에서 사용
├── config.json
├── CMakeLists.txt
└── CLAUDE.md
```