# 개발 가이드

## 프로젝트 구조

```
tunnel-proxy/
├── include/          # 헤더 파일
│   ├── core/        # 핵심 프록시 로직
│   └── utils/       # 유틸리티 클래스
├── src/             # 소스 파일
│   └── utils/       # 유틸리티 구현
├── tests/           # 테스트 코드
├── scripts/         # 빌드/배포 스크립트
├── config/          # 설정 파일
└── docs/            # 문서
```

## 코딩 규칙

### 네이밍
- 클래스: PascalCase (예: `BasicProxy`)
- 함수/변수: snake_case (예: `handle_connection`)
- 멤버 변수: snake_case + 접미사 `_` (예: `listen_fd_`)
- 상수: UPPER_SNAKE_CASE (예: `MAX_BUFFER_SIZE`)

### 헤더 가드
```cpp
#pragma once  // 사용
```

### 네임스페이스
```cpp
namespace proxy {
// 모든 코드는 proxy 네임스페이스 안에
}
```

### 메모리 관리
- ❌ `new`/`delete` 직접 사용 금지
- ✅ `std::unique_ptr`, `std::shared_ptr` 사용
- ✅ RAII 패턴 준수

### 에러 처리
```cpp
// 예외 사용
throw std::runtime_error("Error message");

// 에러 로깅
Logger::error("Error occurred");
```

## 개발 워크플로우

### 1. 브랜치 생성
```bash
git checkout -b feature/새기능
```

### 2. 개발 및 테스트
```bash
# 빌드
./scripts/build.sh Debug

# 테스트
./scripts/test.sh

# 실행
cd build
./proxy --config ../config/config.json
```

### 3. 커밋
```bash
git add .
git commit -m "feat: 새 기능 추가"
```

#### 3-1. 커밋 관련
```bash
fix : 버그 수정
build : 빌드 관련 파일 수정, 모듈 설치 또는 삭제에 대한 커밋
chore : 그 외 자잘한 수정
docs : 문서 수정
test : 테스트 코드 수정
refactor : 코드 리팩토링에 대한 커밋

```

### 4. Pull Request
- `develop` 브랜치로 PR 생성
- 코드 리뷰 후 머지

## Phase별 작업

### Phase 1: 기본 TCP 프록시 (1-2주)
**서규님:**
- `BasicProxy::create_listening_socket()` 구현
- `BasicProxy::handle_connection()` 구현
- `BasicProxy::forward_data()` 구현

**신입:**
- `Config::load_from_file()` 완성
- `Logger` 파일 출력 기능
- 테스트 코드 작성

### Phase 2: epoll 비동기 I/O (2-3주)
**서규님:**
- `EpollProxy` 전체 구현
- Non-blocking I/O 처리

**신입:**
- `Statistics` 클래스 구현
- 벤치마크 스크립트
- 문서 작성

### Phase 3-5: 추후 계획
- TLS 암호화
- UDP 지원
- Zero-copy 최적화

## 디버깅 팁

### GDB 사용
```bash
gdb ./proxy
(gdb) run --config ../config/config.json
(gdb) bt  # backtrace
```

### Valgrind 메모리 체크
```bash
valgrind --leak-check=full ./proxy
```

### strace 시스템 콜 추적
```bash
strace -e trace=network ./proxy
```

## 참고 자료
- A Tour of C++ (Chapter 1-6)
- man pages: `man 2 socket`, `man 7 epoll`
- cppreference.com
