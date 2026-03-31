# tunnel-proxy

폐쇄망 내부 리소스에 브라우저만으로 안전하게 접근할 수 있게 해주는 리버스 터널 게이트웨이.

VPN 없이 NAT/방화벽 뒤 환경에 접근할 수 있고, 매 연결마다 mTLS + JWT로 신원을 검증하는 Zero Trust 구조다.
원격 데스크톱(RDP/VNC), 터미널(SSH), 웹 페이지(HTTPS headless)를 모두 브라우저 단일 창에서 제어할 수 있다.

> 웹 클라이언트(브라우저 앱)는 별도 레포로 관리한다. 이 레포는 게이트웨이 서버만 담당한다.

---

## 전체 구조

```
[ 브라우저 ]
     ↕ WSS:443 (WebSocket Secure)
[ tunnel-proxy 게이트웨이 ]
     ├─ mTLS + JWT + AccessPolicy  ← Zero Trust 인증 레이어
     ├─ Guacamole 프로토콜         ← RDP / VNC / SSH 렌더링
     ├─ webkit headless 스트리밍   ← HTTPS 웹페이지 렌더링
     └─ 리버스 터널                ← NAT 뒤 에이전트로 연결
              ↕ TCP (역방향 연결 유지)
         [ TunnelAgent ]  ←── NAT/방화벽 뒤 내부 환경
              ↕
         [ 내부 타겟: RDP 서버 / VNC 서버 / SSH 서버 / 내부 웹 서비스 ]
```

---

## 왜 리버스 터널인가

일반적인 터널은 서버가 클라이언트에 연결한다. 폐쇄망 환경에서는 외부에서 내부로 들어오는 연결이 방화벽에 막힌다.
리버스 터널은 반대로 **내부 에이전트가 먼저 게이트웨이 서버에 연결을 맺고 유지**한다.
외부 요청이 들어오면 이미 열려있는 터널을 역방향으로 이용해 내부 리소스에 도달한다.

```
외부 요청 → 게이트웨이 → (기존 터널 역방향) → 에이전트 → 내부 타겟
                ↑
         에이전트가 먼저 연결해둔 상태
```

---

## Zero Trust 인증

네트워크 위치를 신뢰하지 않는다. 내부망에서 온 요청도 동일하게 검증한다.

```
연결
 ↓
mTLS 핸드셰이크 — CA 서명 클라이언트 인증서 검증 (장치 인증)
 ↓
JWT verify     — Bearer 토큰 서명/만료 검증 (사용자 인증)
 ↓
AccessPolicy   — subject / tunnel_id / target_ip / port 규칙 매칭 (리소스 접근 제어)
 ↓
허용 or 차단 (매칭 규칙 없으면 default-deny)
```

---

## 원격 프로토콜 지원

### Guacamole (RDP / VNC / SSH)

Apache Guacamole 프로토콜을 게이트웨이에서 직접 구현한다.
서버가 RDP/VNC/SSH 세션을 열고 화면을 렌더링한 뒤, Guacamole instruction 스트림으로 브라우저에 전달한다.
브라우저는 키보드/마우스 이벤트를 역방향으로 전송해 원격 데스크톱/터미널을 제어한다.

Guacamole 프로토콜은 길이 접두사 텍스트 포맷이다:
```
4.draw,1.0,2.10,3.100,3.200;   ← opcode.length.arg, ...
```

### HTTP 웹 프록시 (web 프로토콜)

Guacamole는 HTTP/HTTPS 웹페이지를 기본 지원하지 않는다.
`web` 커스텀 프로토콜로 이를 확장한다.

브라우저가 `connect,web,https://내부서버/path;`를 보내면
게이트웨이가 해당 URL을 서버 측에서 직접 요청하고 결과를 `response` instruction으로 반환한다.

```
브라우저
  connect,web,https://내부서버/path;
        ↓
  GuacWebSocketGateway
        ├─ web_renderer=http (기본)
        │      → GuacHttpClient(libcurl) → HTTP GET
        │      → response,200,text/html,<base64>;
        └─ web_renderer=chromium (선택)
               → GuacWebClient → Chromium headless CDP
               → delta frame 스트리밍
```

새 Guacamole instruction:
```
response,200,text/html,<base64-encoded-body>;
```

`config.json`의 `web_renderer` 필드로 구현체를 선택한다:
```json
{ "web_renderer": "http" }      // 기본 — libcurl 직접 요청, 경량
{ "web_renderer": "chromium" }  // JS 실행이 필요한 SPA 등
```

---

## 구성 요소

### TunnelAgent
NAT/방화벽 뒤 환경에서 게이트웨이 서버로 TCP 연결을 먼저 맺고 유지한다.
서버로부터 OPEN 메시지를 받으면 내부 타겟으로 연결을 중계한다.
heartbeat 타임아웃으로 단절을 감지하고 지수 백오프로 자동 재연결한다.

### TunnelServer
에이전트 연결을 수신하고 session_id를 발급해 세션 맵을 관리한다.
외부 요청이 들어오면 해당 에이전트 터널로 OPEN 메시지를 보내 세션을 개설하고 양방향 데이터를 중계한다.

### MtlsContext
CA가 서명한 클라이언트 인증서를 검증한다. TLS 1.2 이상만 허용한다.
인증 성공 후 CN(Common Name)을 추출해 AccessPolicy의 subject로 사용한다.

### JwtVerifier
HTTP Authorization 헤더의 Bearer 토큰 서명을 검증한다.
HS256(HMAC-SHA256), RS256(RSA-SHA256)을 지원한다.
만료(exp), 알고리즘 불일치, 서명 위조를 모두 거부한다.

### AccessPolicy
subject / tunnel_id / target_ip / target_port 4개 필드로 접근을 제어한다.
first-match 방식이며 매칭 규칙이 없으면 default-deny로 차단된다.
`*` 와일드카드를 지원하고 JSON 파일로 정책을 로드한다.

### EpollProxy
단일 스레드 epoll ET 모드로 다수의 TCP 연결을 처리한다.
splice 시스템 콜로 커널-유저 간 메모리 복사 없이 데이터를 전달한다.

### TlsProxy
OpenSSL 기반 TLS 암호화 레이어. 핸드셰이크, SSL_read/SSL_write 논블로킹 처리를 담당한다.

### UdpProxy
SOCK_DGRAM 기반 UDP 포워딩.
(src_ip, src_port) → target_fd 세션 테이블로 클라이언트를 분리하고 만료된 세션을 자동 정리한다.

---

## 터널 프로토콜

에이전트와 게이트웨이 서버 간 통신은 16바이트 고정 헤더 바이너리 프로토콜을 사용한다.

```
[ magic(4) | type(1) | flags(1) | reserved(2) | session_id(4) | length(4) ] + payload
  0x544E4C50                                    0=컨트롤 / 1+=터널 세션
```

| 타입 | 방향 | 설명 |
|------|------|------|
| HELLO / HELLO_ACK | 에이전트 → 서버 | 최초 등록 (agent_id 전달) |
| OPEN / OPEN_ACK | 서버 → 에이전트 | 세션 개설 (target_ip:port 전달) |
| DATA | 양방향 | 페이로드 중계 |
| CLOSE | 양방향 | 세션 종료 |
| HEARTBEAT / HEARTBEAT_ACK | 양방향 | 연결 유지 확인 |

---

## 빌드

```bash
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Debug ..
make -j$(nproc)
```

외부 의존성은 cmake 시점에 FetchContent로 자동 다운로드된다. apt 설치 불필요.

빌드 결과물:

| 바이너리 | 용도 |
|----------|------|
| `build/proxy` | 게이트웨이 서버 |
| `build/agent` | 리버스 터널 에이전트 (NAT 뒤 머신에 배포) |

---

## 워크플로우

### 기본 구성 — 브라우저 원격 접속

공개 서버에 `proxy`를 올리면 브라우저에서 SSH/RDP/VNC/웹에 바로 접속할 수 있다.

```
브라우저
  │ WebSocket (ws://서버IP:8765)
  ▼
┌─────────────────────────────────────────────┐
│  proxy (공개 서버)                           │
│                                             │
│  GuacWebSocketGateway :8765                 │
│   ├─ ssh ──────────────────────▶ SSH 서버   │
│   ├─ rdp ──────────────────────▶ RDP 서버   │
│   ├─ vnc ──────────────────────▶ VNC 서버   │
│   └─ web ──▶ GuacHttpClient ────▶ URL       │
└─────────────────────────────────────────────┘
```

---

### 리버스 터널 구성 — NAT 뒤 내부망 접근

내부 서버에 직접 들어갈 수 없을 때. 에이전트가 먼저 서버에 연결을 맺어두고, 서버가 그 연결을 역방향으로 이용한다.

```
브라우저 / 외부 클라이언트
  │ :8765 (Guacamole) 또는 :9901 (TCP 터널)
  ▼
┌──────────────────────────────────────────────────────┐
│  proxy (공개 서버)                                    │
│                                                      │
│  TunnelServer :9900 ◀─────────────────────────────┐  │
│  (에이전트 대기)                                    │  │
│                                                    │  │
│  외부 클라이언트 :9901 ──▶ 터널 ──────────────────┐│  │
└────────────────────────────────────────────────────┼┼─┘
                                                     ││
                              역방향 TCP 연결 (유지) ││
                                                     ▼│
                                          ┌───────────┴──────────┐
                                          │  agent (NAT 뒤)       │
                                          │  TunnelAgent          │
                                          │   └──▶ 내부 서버:PORT │
                                          └──────────────────────┘
```

---

## 환경 설정 및 사용 방법

### 1. 빌드

```bash
git clone https://github.com/skrevolve/tunnel-proxy
cd tunnel-proxy
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..   # Chromium 자동 설치 포함
make -j$(nproc)
```

### 2. 서버 실행 (공개 서버)

```json
// config.json
{
  "agent_port":   9900,
  "proxy_port":   9901,
  "guac_port":    8765,
  "web_renderer": "http"   // "http"(기본) 또는 "chromium"
}
```

```bash
./proxy
# :9900  에이전트 역방향 연결 대기 (TunnelServer)
# :9901  외부 클라이언트 터널 진입 (TunnelServer)
# :8765  브라우저 WebSocket 연결  (GuacWebSocketGateway)
```

### 3. 에이전트 실행 (NAT 뒤 머신)

```json
// agent.json
{
  "server_ip":   "공개서버_IP",
  "server_port": 9900,
  "agent_id":    "my-agent"
}
```

```bash
./agent -c agent.json   # 지수 백오프 자동 재연결 포함
```

### 4. 브라우저 접속

```bash
cd frontend && npm install && npm run dev
# http://localhost:5173
```

| 항목 | 값 |
|------|----|
| 게이트웨이 URL | `ws://서버IP:8765` |
| 프로토콜 | SSH / RDP / VNC / WEB 중 선택 |
| 호스트/포트 | 접속 대상 서버 (WEB은 URL 입력) |

---

## 테스트

```bash
cd build && ctest --output-on-failure
```

| 경로 | 대상 |
|------|------|
| `tests/phase1/` | Config JSON 파싱, Logger |
| `tests/phase2/` | EpollProxy 비동기 I/O |
| `tests/phase3/` | splice zero-copy 처리량 벤치마크 |
| `tests/phase4/` | TLS 인증서 유효/만료/위조 검증 |
| `tests/phase5/` | UDP 에코, 세션 분리, 타임아웃 |
| `tests/phase6/` | 터널 프레임 직렬화/파싱/검증 |
| `tests/phase7/` | mTLS CN 추출, JWT 위조·만료·알고리즘 불일치, AccessPolicy |

각 Phase별 시나리오 설명은 `tests/phaseN/phaseN.md` 를 참고한다.

---

## 의존성

| 항목 | 버전 | 관리 방법 |
|------|------|-----------|
| CMake | 3.14+ | 시스템 |
| GCC | 11+ (C++17) | 시스템 |
| OpenSSL | 3.x | 시스템 (find_package) |
| libcurl | 7.x+ | 시스템 (find_package) |
| nlohmann/json | v3.11.3 | FetchContent |
| Google Test | v1.14.0 | FetchContent |
| Chromium | 최신 stable | 선택 — `web_renderer=chromium` 사용 시 자동 탐지/설치 |

---

## 디렉토리 구조

```
tunnel-proxy/
├── include/
│   ├── core/
│   │   ├── basic_proxy.h       # TCP 멀티스레드 프록시
│   │   ├── epoll_proxy.h       # epoll ET + splice zero-copy
│   │   ├── tls_proxy.h         # OpenSSL TLS 프록시
│   │   ├── udp_proxy.h         # UDP 세션 테이블 프록시
│   │   ├── tunnel_protocol.h   # 바이너리 프레임 프로토콜
│   │   ├── tunnel_agent.h      # 역방향 연결 에이전트
│   │   ├── tunnel_server.h     # 에이전트 수신 + 세션 라우터
│   │   ├── mtls_context.h      # mTLS SSL_CTX + CN 추출
│   │   ├── jwt_verifier.h      # HS256/RS256 JWT 검증
│   │   ├── access_policy.h     # 접근 제어 정책 (first-match)
│   │   └── guac_http.h         # libcurl HTTP 클라이언트 (web 프로토콜 기본 구현)
│   └── utils/
│       ├── config.h
│       └── logger.h
├── src/
│   ├── main.cpp
│   ├── basic_proxy.cpp
│   ├── epoll_proxy.cpp
│   ├── tls_proxy.cpp
│   ├── udp_proxy.cpp
│   ├── tunnel_protocol.cpp
│   ├── tunnel_agent.cpp
│   ├── tunnel_server.cpp
│   ├── mtls_context.cpp
│   ├── jwt_verifier.cpp
│   ├── access_policy.cpp
│   └── utils/
│       ├── config.cpp
│       └── logger.cpp
├── tests/
│   ├── phase1/ … phase7/       # 각 phaseN.md에 시나리오 설명
├── scripts/
│   ├── build.sh
│   ├── test.sh
│   └── gen_cert.sh
├── config.json
└── CMakeLists.txt
```
