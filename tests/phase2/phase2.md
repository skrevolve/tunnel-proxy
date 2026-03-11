# Phase 2 테스트 가이드

Phase 1과 같은 설정, 같은 nc 명령을 사용하지만 검증 포인트가 다르다.
Phase 1은 "데이터가 오가는가"를 봤다면, Phase 2는 "연결 자원이 제대로 정리되는가"를 본다.

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

---

## 시나리오 1 — 연결 해제 전파 (close_connection 검증)

클라이언트가 끊으면 타겟 쪽 fd도 함께 닫히는지 확인한다.
`close_connection()`이 양쪽 fd를 모두 정리하지 않으면 타겟 nc는 계속 살아있는다.

```bash
# 터미널 1 — 타겟
nc -l -p 8000

# 터미널 2 — 프록시
cd ~/tunnel-proxy/build
./proxy --config ../config.json

# 터미널 3 — 클라이언트
nc localhost 8080
```

터미널 3에서 `Ctrl+C` 입력.

**검증**: 터미널 1의 `nc`도 함께 종료되면 성공.
터미널 1이 살아있으면 `close_connection(peer_fd)`가 빠진 것.

---

## 시나리오 2 — 대용량 데이터 전송 (partial write 검증)

EpollProxy의 `forward_data()`는 `EAGAIN`까지 읽고, partial write 루프로 쓴다.
데이터가 중간에 잘리지 않는지 바이트 수로 검증한다.

```bash
# 터미널 1 — 타겟 (받은 데이터를 파일로 저장)
nc -l -p 8000 > /tmp/received.bin

# 터미널 2 — 프록시
./proxy --config ../config.json

# 터미널 3 — 1MB 전송
dd if=/dev/urandom bs=1M count=1 | nc localhost 8080
```

전송 완료 후:

```bash
wc -c /tmp/received.bin
```

**검증**: `1048576` 이면 1바이트도 유실 없이 전달된 것.

---

## 시나리오 3 — 프록시 로그에서 연결 카운터 확인

`verbose: true` 상태에서 프록시를 실행하면 연결마다 로그가 찍힌다.

```bash
./proxy --config ../config.json
```

nc로 연결 후 끊으면 아래 순서로 로그가 나와야 한다:

```
[INFO] new connection (client_fd=5 target_fd=6) total=1
[DEBUG] connection closed (fd=5 peer_fd=6)
```

**검증**: `total`이 연결마다 증가하고, 종료 시 `connection closed`가 찍히면
`accept_connection()` → `close_connection()` 흐름 정상.
