# CLAUDE.md — tunnel-proxy

## 프로젝트 목표
OtoRAS 학습 및 제작:
리버스 터널 → Zero Trust 인증 → Guacamole 서버사이드 렌더링

최종 목표: Cloudflare Tunnel + Apache Guacamole 유사 시스템을 C++로 직접 구현

---

## 세션 작업 단위 (토큰 절약)

**한 세션에 하나의 작업 단위만 처리.**
세션 시작 시 아래 목록에서 현재 작업 번호를 확인하고 해당 작업만 완료.

### Phase 1 — 기본 TCP 프록시 (멀티스레드)

| 세션 | 브랜치 | 작업 내용 |
|------|--------|-----------|
| 1-A | `feat/cmake-json` | CMakeLists.txt에 nlohmann/json FetchContent 추가 |
| 1-B | `feat/config-parse` | Config::load_from_file() JSON 실제 파싱 구현 |
| 1-C | `feat/socket-listen` | BasicProxy::create_listening_socket() + run() accept 루프 |
| 1-D | `feat/forward-data` | connect_to_target() + handle_connection() + forward_data() |
| 1-E | `refactor/cleanup` | proxy.h 삭제, running_ → atomic<bool>, 테스트 보강 |

### Phase 2 — epoll 비동기 I/O
| 세션 | 브랜치 | 작업 내용 |
|------|--------|-----------|
| 2-A | `feat/epoll-proxy` | EpollProxy 클래스 skeleton + non-blocking 소켓 |
| 2-B | `feat/epoll-loop` | epoll_wait 이벤트 루프 구현 |
| 2-C | `feat/epoll-forward` | edge-triggered 양방향 데이터 포워딩 |

### 이후 Phase (Phase 3~8)
추후 세션 단위 분리 예정:
- Phase 3: Zero-copy (splice/sendfile)
- Phase 4: TLS 암호화 (OpenSSL)
- Phase 5: UDP 지원
- Phase 6: 리버스 터널 프로토콜
- Phase 7: Zero Trust 인증 (mTLS/JWT)
- Phase 8: Guacamole 프로토콜 연동

---

## 세션 시작 방법

세션 시작 시 아래 순서로 진행:

```
1. 위 표에서 현재 세션 번호 확인
2. 해당 브랜치 생성
3. 작업 내용만 구현
4. 빌드 확인
5. PR 생성 후 종료
```

**해당 세션 범위를 벗어나는 작업은 하지 않음.**
다음 세션 작업이 보여도 건드리지 않고 TODO 주석만 남김.

---

## 의존성 규칙

**apt, brew, yum 등 시스템 패키지 매니저 절대 사용 금지.**
모든 외부 라이브러리는 CMakeLists.txt의 FetchContent로 관리.
환경(WSL, Ubuntu, Docker, 서버)이 바뀌어도 cmake 한 번으로 동일하게 동작해야 함.

시스템 의존성 (find_package로만 탐지):
- CMake 3.14+
- GCC 11+ (C++17)
- OpenSSL 3.x
- pthread

FetchContent로 관리:
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

새 라이브러리 추가 시 apt 절대 사용하지 말고 위 방식으로 CMakeLists.txt에 추가.

---

## 빌드 규칙

```bash
cd build
cmake -DCMAKE_BUILD_TYPE=Debug .. && make -j$(nproc)
```

- 빌드 실패 시 에러 읽고 수정 → 재빌드 확인까지 완료
- 빌드 실패 상태로 커밋 절대 금지

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

## Git 워크플로우 (Issue → Branch → PR)

### 1. 이슈 생성
세션 시작 전 GitHub에 이슈 생성:
```bash
gh issue create \
  --title "[세션번호] 작업내용" \
  --body "## 작업 내용\n- 구현 항목\n\n## 완료 기준\n- 빌드 성공\n- 테스트 통과" \
  --label "enhancement"
```

예시:
```bash
gh issue create \
  --title "[1-C] BasicProxy 소켓 생성 및 accept 루프 구현" \
  --body "## 작업 내용\n- create_listening_socket() 구현\n- run() accept 루프 구현\n\n## 완료 기준\n- 빌드 성공\n- nc로 연결 테스트 통과" \
  --label "enhancement"
```

### 2. 브랜치 생성
```bash
git checkout -b feat/socket-listen
```

### 3. 구현 + 빌드 확인

### 4. 커밋
```bash
git add -A
git commit -m "<type>(<scope>): <description>"
```

커밋 타입:
- `feat`: 새 기능
- `fix`: 버그 수정
- `perf`: 성능 개선
- `refactor`: 리팩토링
- `test`: 테스트
- `docs`: 문서
- `build`: 빌드/의존성

커밋 예시:
- `build(cmake): add nlohmann/json via FetchContent`
- `feat(config): implement JSON parsing with nlohmann/json`
- `feat(proxy): implement create_listening_socket with SO_REUSEADDR`
- `feat(proxy): implement forward_data bidirectional tunnel`

### 5. PR 생성 후 master 머지
```bash
git push origin feat/socket-listen

gh pr create \
  --title "[1-C] BasicProxy 소켓 생성 및 accept 루프 구현" \
  --body "closes #<이슈번호>\n\n## 변경 사항\n- create_listening_socket() 구현\n- run() accept 루프 구현\n\n## 테스트\n- 빌드 성공\n- nc 양방향 통신 확인" \
  --base master

gh pr merge --squash --delete-branch
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