# CLAUDE.md — tunnel-proxy

## 프로젝트 목표

OtoRAS라는 제품의 VPN/원격접속 기술을 학습하고 직접 구현하는 프로젝트.

구현 목표 (단계별):
1. **리버스 터널** — 클라이언트가 NAT 뒤에 있어도 서버에서 연결 가능하게
2. **Zero Trust 인증** — 네트워크 신뢰 없이 매 요청마다 인증 (mTLS/JWT)
3. **Guacamole 프로토콜** — 서버에서 직접 렌더링해서 브라우저로 전달 (RDP/VNC/SSH 웹화)

최종 결과물: Cloudflare Tunnel + Apache Guacamole를 C++로 직접 만든 것

---

## 현재 개발 단계

**Phase 1 진행 중** — 기본 TCP 프록시 (멀티스레드)

코드는 skeleton만 있고 핵심 함수들이 전부 미구현 상태.
Phase 1이 완성되어야 이후 epoll, TLS, 리버스 터널로 발전 가능.

---

## 세션 작업 단위 (토큰 절약)

**한 세션에 하나의 작업 단위만 처리.**
세션 시작 시 현재 세션 번호를 전달받으면 해당 작업만 완료하고 PR까지 마무리.
다음 세션 범위는 TODO 주석만 남기고 절대 건드리지 않음.

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

### Phase 3~8 (추후 세션 단위 분리 예정)

| Phase | 작업 | 왜 필요한가 |
|-------|------|-------------|
| Phase 3 | Zero-copy (splice/sendfile) | 커널-유저스페이스 메모리 복사 제거. 대용량 데이터 터널링 성능 향상 |
| Phase 4 | TLS 암호화 (OpenSSL) | Zero Trust의 기반. 터널 내 데이터를 암호화하고 인증서로 신원 확인 |
| Phase 5 | UDP 지원 | VPN 트래픽의 상당수가 UDP. 게임/영상통화 등 지연 민감한 트래픽 처리 |
| Phase 6 | 리버스 터널 프로토콜 | OtoRAS 핵심. 클라이언트가 먼저 서버에 연결해두면 서버가 그 터널로 요청을 역방향 전달 |
| Phase 7 | Zero Trust 인증 (mTLS/JWT) | 터널 접근 자체를 인증. 네트워크 레벨 신뢰 없이 앱/사용자 단위로 접근 제어 |
| Phase 8 | Guacamole 프로토콜 연동 | 서버에서 RDP/VNC/SSH 세션을 렌더링해서 브라우저로 스트리밍. 클라이언트에 별도 앱 불필요 |

---

## Git 워크플로우 (커밋별 PR)

커밋마다 feature 브랜치 → PR → merge. 이슈는 생성하지 않음.

### 1. 브랜치 생성

```bash
git checkout -b <type>/<scope>
# 예: feat/epoll-init, fix/accept-loop
```

### 2. 구현 + 빌드 확인

```bash
cd build && cmake -DCMAKE_BUILD_TYPE=Debug .. && make -j$(nproc)
```

### 3. 커밋

```bash
git add <파일>
git commit -m "<type>(<scope>): <description>"
```

커밋 타입: `feat` / `fix` / `perf` / `refactor` / `test` / `docs` / `build`

### 4. PR 생성 및 머지

```bash
git push origin <브랜치명>
gh pr create --title "<커밋 제목>" --body "$(cat <<'EOF'
## 변경 사항
- 항목1
- 항목2

## 빌드
\`\`\`
[100%] Built target proxy
\`\`\`
EOF
)" --base master
gh pr merge --squash --delete-branch
git checkout master && git pull origin master
```

PR 본문 필수 항목:
- **변경 사항**: 구현한 항목 목록
- **빌드**: 성공 출력 결과

---

## 의존성 규칙

**apt, brew, yum 등 시스템 패키지 매니저 절대 사용 금지.**
이유: WSL/Ubuntu/Docker/서버 등 환경마다 패키지 이름과 버전이 달라 의존성이 깨짐.
모든 외부 라이브러리는 CMakeLists.txt의 FetchContent로 관리.

시스템 의존성 (find_package로만 탐지):
- CMake 3.14+
- GCC 11+ (C++17)
- OpenSSL 3.x
- pthread

FetchContent로 관리하는 라이브러리:
- nlohmann/json v3.11.3

FetchContent 추가 방법:
```cmake
include(FetchContent)
FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp
    DOWNLOAD_NO_EXTRACT TRUE
)
FetchContent_MakeAvailable(nlohmann_json)
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

Phase 1 동작 확인:
```bash
# 터미널 1: nc -l 80
# 터미널 2: ./proxy --config ../config.json
# 터미널 3: nc localhost 8080
# → 터미널 1↔3 양방향 데이터 전달 확인
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
│   │   ├── basic_proxy.h
│   │   └── epoll_proxy.h       # Phase 2 (예정)
│   └── utils/
│       ├── config.h
│       └── logger.h
├── src/
│   ├── main.cpp
│   ├── basic_proxy.cpp
│   └── utils/
│       ├── config.cpp
│       └── logger.cpp
├── tests/
│   ├── CMakeLists.txt
│   └── test_logger.cpp
├── scripts/
│   ├── build.sh
│   ├── test.sh
│   └── gen_cert.sh
├── config.json
├── CMakeLists.txt
└── CLAUDE.md
```