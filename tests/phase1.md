# Phase 1 테스트 가이드

## 환경 설정

`config.json` 기준:

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

> port 80은 시스템이 점유 중이거나 root 권한이 필요하므로 `target_port`는 8000 이상 사용

---

## nc (netcat) 란?

TCP/UDP 소켓을 터미널에서 직접 열고 닫을 수 있는 네트워크 유틸리티.  
"네트워크의 cat"이라는 의미로, 소켓에 데이터를 읽고 쓰는 게 전부.

```
# 서버 모드 — 포트를 열고 연결 대기
nc -l -p 8000

# 클라이언트 모드 — 해당 주소:포트로 연결
nc localhost 8080
```

시나리오에서 각 역할:

```
[nc -l -p 8000]  ←→  [./proxy 8080→8000]  ←→  [nc localhost 8080]
   타겟 서버               프록시                    클라이언트
```

- **왼쪽 nc**: 실제 서비스 자리 (나중엔 nginx, ssh 등으로 교체)
- **오른쪽 nc**: 브라우저나 클라이언트 앱 자리
- **지금은 둘 다 nc로 대체**해서 프록시가 중간에서 데이터를 잘 전달하는지만 검증

---

## 시나리오 1 — nc 양방향 채팅

터미널 3개 필요.

```bash
# 터미널 1 — 타겟 서버 역할
nc -l -p 8000

# 터미널 2 — 프록시 실행
cd ~/tunnel-proxy/build
./proxy --config ../config.json

# 터미널 3 — 클라이언트
nc localhost 8080
```

**검증**: 터미널 3에서 입력 → 터미널 1에 출력, 반대도 동일하면 `forward_data` + `handle_connection` 정상 동작.

종료: `Ctrl+C`

---

## 시나리오 2 — HTTP 프록시

```bash
# 터미널 1 — Python 간이 HTTP 서버
python3 -m http.server 8000

# 터미널 2 — 프록시 실행
./proxy --config ../config.json

# 터미널 3 — curl로 프록시 통해 접근
curl http://localhost:8080
```

**검증**: HTML 파일 목록이 출력되면 HTTP 트래픽 포워딩 성공.

---

## 시나리오 3 — 다중 연결 동시 처리 (멀티스레드 검증)

```bash
# 터미널 1 — 타겟 서버 (-k: 연결 끊겨도 유지)
nc -l -k -p 8000

# 터미널 2 — 프록시 실행
./proxy --config ../config.json

# 터미널 3 — 동시 연결 4개
for i in {1..4}; do echo "client $i" | nc localhost 8080 & done; wait
```

**검증**: 프록시 로그에 `total=4` 가 찍히면 멀티스레드 accept 루프 정상 동작.

---

## 시나리오 4 — Graceful Shutdown

프록시 실행 중 `Ctrl+C` 입력.

**예상 출력**:

```
[INFO] Received signal 2, shutting down...
[INFO] Proxy stopped
[INFO] Total connections: N
```