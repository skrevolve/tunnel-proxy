# CLAUDE.md — tunnel-proxy

## 프로젝트 목표
OtoRAS 학습 및 제작:
리버스 터널 → Zero Trust 인증 → Guacamole 서버사이드 렌더링

최종 목표: Cloudflare Tunnel + Apache Guacamole 유사 시스템을 C++로 직접 구현

---

## 현재 개발 단계

**Phase 1 진행 중** — 기본 TCP 프록시 (멀티스레드)

미구현 목록 (우선순위 순):
1. `CMakeLists.txt` — nlohmann/json FetchContent 추가
2. `Config::load_from_file()` — 실제 JSON 파싱 구현
3. `BasicProxy::create_listening_socket()` — socket/setsockopt/bind/listen
4. `BasicProxy::run()` — accept() 루프
5. `BasicProxy::connect_to_target()` — socket/connect
6. `BasicProxy::handle_connection()` — 양방향 스레드 생성
7. `BasicProxy::forward_data()` — read/write 루프
8. `proxy.h` — BasicProxy와 중복이므로 삭제
9. `running_` — `std::atomic<bool>`로 변경

Phase 순서:
- Phase 1: 기본 TCP 프록시 (멀티스레드)
- Phase 2: epoll 기반 비동기 I/O
- Phase 3: Zero-copy 최적화 (splice/sendfile)
- Phase 4: TLS 암호화 (OpenSSL)
- Phase 5: UDP 지원
- Phase 6: 리버스 터널 프로토콜
- Phase 7: Zero Trust 인증 (mTLS/JWT)
- Phase 8: Guacamole 프로토콜 연동

---

## 의존성 규칙

**apt, brew, yum 등 시스템 패키지 매니저 절대 사용 금지.**
모든 외부 라이브러리는 CMakeLists.txt의 FetchContent로 관리.
환경(WSL, Ubuntu, Docker, 서버 등)이 바뀌어도 cmake 한 번으로 동일하게 동작해야 함.

현재 의존성:
- CMake 3.14+ (FetchContent 안정 버전)
- GCC 11+ (C++17)
- OpenSSL 3.x (시스템 설치, find_package로 탐지)
- pthread (시스템)

FetchContent로 관리하는 라이브러리:
- nlohmann/json v3.11.3

CMakeLists.txt FetchContent 템플릿:
```cmake
include(FetchContent)

FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp
    DOWNLOAD_NO_EXTRACT TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

# 또는 헤더온리 단일파일 방식:
# file(DOWNLOAD
#     https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp
#     ${CMAKE_SOURCE_DIR}/include/nlohmann/json.hpp
# )
```

새 라이브러리 추가 시 apt 사용하지 말고 위 방식으로 CMakeLists.txt에 추가.

---

## 빌드 규칙

작업 후 반드시 아래 순서로 실행:

```bash
cd build
cmake -DCMAKE_BUILD_TYPE=Debug .. && make -j$(nproc)
```

빌드 실패 시:
- 에러 메시지 읽고 원인 파악 후 수정
- 재빌드로 성공 확인까지 완료
- 빌드 실패 상태로 커밋 절대 금지

---

## 테스트 규칙

```bash
cd build && ctest --output-on-failure
```

테스트 실패 시 수정 후 재시도. 테스트 실패 상태로 커밋 금지.

Phase 1 동작 확인 방법:
```bash
# 터미널 1
nc -l 80

# 터미널 2
./proxy --config ../config.json

# 터미널 3
nc localhost 8080
# → 터미널 1과 3 사이에 데이터 양방향 전달되면 성공
```

---

## Git 자동화 규칙

빌드 + 테스트 통과 후 반드시 실행:

```bash
git add -A
git commit -m "<type>(<scope>): <description>"
git push origin master
```

커밋 타입:
- `feat`: 새 기능
- `fix`: 버그 수정
- `perf`: 성능 개선
- `refactor`: 리팩토링
- `test`: 테스트 추가/수정
- `docs`: 문서
- `build`: 빌드 시스템/의존성

커밋 예시:
- `build(cmake): add nlohmann/json via FetchContent`
- `feat(config): implement JSON parsing with nlohmann/json`
- `feat(proxy): implement create_listening_socket with SO_REUSEADDR`
- `feat(proxy): implement forward_data bidirectional tunnel`
- `refactor(proxy): change running_ to std::atomic<bool>`

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
- RAII 패턴 준수 (소켓 fd도 RAII로 감싸기)

**네트워크**:
- 모든 시스템 콜 리턴값 반드시 체크 (`errno` 확인)
- 소켓 close 누락 금지 — RAII 래퍼 또는 finally 패턴 사용
- Phase 2부터 non-blocking + edge-triggered epoll 기본

**에러 처리**:
```cpp
if (fd < 0) {
    throw std::runtime_error("socket: " + std::string(strerror(errno)));
}
```

**헤더 가드**: `#pragma once` 사용

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