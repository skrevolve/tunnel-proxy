# Tunnel Proxy

고성능 TCP/UDP 터널링 프록시 프로젝트

## 프로젝트 개요

암호화된 네트워크 터널을 통해 안전하게 데이터를 전달하는 프록시 시스템

## 개발 단계

- [x] Phase 0: 프로젝트 구조 설정
- [ ] Phase 1: 기본 TCP 프록시 (멀티스레드)
- [ ] Phase 2: epoll 기반 비동기 I/O
- [ ] Phase 3: Zero-copy 최적화
- [ ] Phase 4: TLS 암호화
- [ ] Phase 5: UDP 지원

## 빌드 방법

```bash
# 빌드
mkdir build && cd build
cmake ..
make -j$(nproc)

# 실행
./proxy --config ../config.json
```

## 요구사항

- CMake 3.10+
- GCC 11+ (C++17)
- OpenSSL 3.x

## 디렉토리 구조

```
tunnel-proxy/
├── src/              # 소스 파일
├── include/          # 헤더 파일
├── tests/            # 테스트 코드
├── scripts/          # 유틸리티 스크립트
├── config.json       # 설정 파일
└── CMakeLists.txt
```

## 기여

- 개발자 A: 코어 엔진
- 개발자 B: 유틸리티 & 도구

## 라이선스

MIT License
