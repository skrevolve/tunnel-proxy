# Phase 3 테스트 가이드

Phase 2까지는 "데이터가 전달되는가"를 봤다.
Phase 3는 splice()로 교체 후 **데이터가 변조 없이 전달되는가**를 검증한다.

splice()는 커널 내부에서 페이지를 이동하기 때문에
유저 공간을 거치지 않는다. 버그가 있으면 데이터가 잘리거나 내용이 바뀔 수 있다.

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

## 시나리오 1 — md5 해시 무결성 검증

splice()가 64KB 청크 경계에서 데이터를 잘라내거나 중복 전송하지 않는지 확인한다.
바이트 수가 맞아도 내용이 달라질 수 있으므로 md5까지 비교한다.

```bash
# 터미널 1 — 타겟 (받은 데이터를 파일로 저장)
nc -l -p 8000 > /tmp/received.bin

# 터미널 2 — 프록시
cd ~/tunnel-proxy/build
./proxy --config ../config.json

# 터미널 3 — 테스트 파일 생성 후 전송
dd if=/dev/urandom bs=1M count=10 of=/tmp/send.bin
nc localhost 8080 < /tmp/send.bin
```

전송 완료 후:

```bash
md5sum /tmp/send.bin /tmp/received.bin
```

**검증**: 두 줄의 해시가 같으면 10MB 전송에서 데이터 변조 없음.
다르면 splice partial 루프 또는 파이프 버퍼 처리에 버그.

---

## 시나리오 2 — 64KB 경계 전송 (파이프 버퍼 크기 검증)

splice()의 청크 크기는 64KB(파이프 기본 버퍼 크기)다.
정확히 64KB, 65KB, 128KB를 각각 보내 경계에서 데이터가 잘리지 않는지 확인한다.

```bash
# 터미널 1
nc -l -p 8000 > /tmp/received.bin

# 터미널 2
./proxy --config ../config.json

# 터미널 3 — 정확히 65536 바이트 (64KB) 전송
dd if=/dev/urandom bs=65536 count=1 of=/tmp/send.bin
nc localhost 8080 < /tmp/send.bin
```

```bash
wc -c /tmp/send.bin /tmp/received.bin
```

**검증**: 두 파일 크기가 `65536`으로 같으면 성공.
65537, 131072 (128KB)로도 반복 확인.

---

## 시나리오 3 — fd 누수 확인 (파이프 생성/소멸 검증)

Phase 3-A는 `forward_data()` 호출마다 파이프를 생성하고 닫는다.
데이터를 전송한 뒤에도 fd 수가 늘어나지 않으면 파이프가 올바르게 닫히는 것.

```bash
# 터미널 1
nc -l -p 8000

# 터미널 2 — 프록시 실행 후 PID 확인
./proxy --config ../config.json &
PROXY_PID=$!

# 터미널 3 — 초기 fd 수 확인
ls /proc/$PROXY_PID/fd | wc -l

# 터미널 3 — 연결 후 데이터 전송
nc localhost 8080
# (몇 가지 메시지 입력 후 Ctrl+C)

# 터미널 3 — 연결 해제 후 fd 수 재확인
ls /proc/$PROXY_PID/fd | wc -l
```

**검증**: 연결 전후 fd 수가 같으면 파이프 fd가 누수되지 않는 것.
연결 중에는 pipe 2개(pipefd[0], pipefd[1])가 일시적으로 늘어날 수 있으나
연결 종료 후에는 원래 수로 돌아와야 한다.
