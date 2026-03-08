# tunnel-proxy

C++17로 구현하는 리버스 터널 + Zero Trust 인증 + Guacamole 프로토콜 프록시.

Cloudflare Tunnel + Apache Guacamole의 동작 원리를 직접 구현하며 학습하는 프로젝트.

---

## 구현 목표

| 단계 | 내용 |
|------|------|
| Phase 1 | 기본 TCP 프록시 (멀티스레드) |
| Phase 2 | epoll 기반 비동기 I/O |
| Phase 3 | Zero-copy 최적화 (splice/sendfile) |
| Phase 4 | TLS 암호화 (OpenSSL) |
| Phase 5 | UDP 지원 |
| Phase 6 | 리버스 터널 프로토콜 |
| Phase 7 | Zero Trust 인증 (mTLS / JWT) |
| Phase 8 | Guacamole 프로토콜 연동 |

---

## 진행 현황

- [x] Phase 1 — 기본 TCP 프록시 (멀티스레드)
- [ ] Phase 2 — epoll 비동기 I/O (진행 중)
- [ ] Phase 3 ~ 8

---

## 빌드

```bash
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Debug ..
make -j$(nproc)
```

> 외부 의존성은 cmake 시점에 FetchContent로 자동 다운로드됩니다. apt 설치 불필요.

---

## 실행

```bash
# config.json 수정 후
./build/proxy --config config.json
```

`config.json` 기본값:

```json
{
  "local_port": 8080,
  "target_ip": "127.0.0.1",
  "target_port": 8000,
  "mode": "tcp",
  "verbose": true,
  "log_file": "proxy.log"
}
```

---

## 테스트

```bash
cd build && ctest --output-on-failure
```

테스트는 Phase별로 구성됩니다:

```
tests/
├── phase1/
│   ├── test_config.cpp   # Config JSON 파싱 테스트
│   └── test_logger.cpp   # Logger 레벨/파일 출력 테스트
└── phase2/               # (Phase 2 구현 후 활성화)
```

---

## 동작 확인 (Phase 1)

터미널 3개로 양방향 포워딩을 직접 확인:

```bash
# 터미널 1 — 타겟 서버 역할
nc -l -p 8000

# 터미널 2 — 프록시 실행
./build/proxy --config config.json

# 터미널 3 — 클라이언트
nc localhost 8080
# 터미널 3에서 입력 → 터미널 1에 출력, 반대도 동일
```

---

## 의존성

| 항목 | 버전 | 관리 방법 |
|------|------|-----------|
| CMake | 3.14+ | 시스템 |
| GCC | 11+ (C++17) | 시스템 |
| OpenSSL | 3.x | 시스템 (find_package) |
| nlohmann/json | v3.11.3 | FetchContent |
| Google Test | v1.14.0 | FetchContent |

---

## 디렉토리 구조

```
tunnel-proxy/
├── include/
│   ├── core/
│   │   ├── basic_proxy.h     # Phase 1
│   │   └── epoll_proxy.h     # Phase 2
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
│   ├── phase1/
│   └── phase2/
├── scripts/
│   ├── build.sh
│   ├── test.sh
│   └── gen_cert.sh
├── config.json
└── CMakeLists.txt
```
