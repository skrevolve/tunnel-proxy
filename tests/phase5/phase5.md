# Phase 5 테스트 가이드

Phase 4까지는 TCP 기반이었다.
Phase 5는 **UDP 패킷이 프록시를 거쳐 올바르게 전달되는가**를 검증한다.
UDP는 연결 개념이 없으므로 세션 분리와 타임아웃이 핵심이다.

---

## nc -u 란?

`nc`에 `-u` 플래그를 붙이면 UDP 소켓으로 동작한다.

```bash
# UDP 서버 모드
nc -u -l -p 9000

# UDP 클라이언트 모드
nc -u localhost 9090
```

시나리오에서 각 역할:

```
[nc -u localhost 9090]  ←→  [UdpProxy 9090→9000]  ←→  [nc -u -l -p 9000]
     UDP 클라이언트              UDP 프록시                   UDP 에코 서버
```

> **참고**: UDP는 연결 종료 신호가 없다. `nc -u` 종료 시 `Ctrl+C`를 사용하고,
> 서버 측은 `-k` 옵션으로 재연결을 허용한다.

---

## 시나리오 1 — 기본 UDP 에코

클라이언트가 보낸 패킷이 프록시를 거쳐 에코 서버에 도달하고, 응답이 클라이언트까지 돌아오는지 확인한다.

```bash
# 터미널 1 — UDP 에코 서버
nc -u -l -p 9000

# 터미널 2 — UdpProxy 실행 (자동화 테스트로 대체)
cd ~/tunnel-proxy/build
ctest -R UdpForward/BasicEcho --output-on-failure

# 직접 확인 시 터미널 3
nc -u localhost 9090
# → "hello" 입력 후 터미널 1에서 "hello" 출력, 터미널 3으로 "hello" 되돌아오면 성공
```

**예상 출력**:

```
[ RUN      ] UdpForward.BasicEcho
[       OK ] UdpForward.BasicEcho
```

**검증**: `recvfrom(listen_fd)` → `send(target_fd)` → `recv(target_fd)` → `sendto(listen_fd, client_addr)` 왕복 정상.

---

## 시나리오 2 — 다중 클라이언트 세션 분리

서로 다른 포트(= 다른 소켓)에서 동시에 패킷을 보낼 때 각자의 응답만 받는지 확인한다.
A의 응답이 B에게 가거나 반대가 되면 세션 테이블이 잘못된 것이다.

```bash
# 터미널 1 — A 클라이언트
nc -u localhost 9090
# → "client-A" 입력

# 터미널 2 — B 클라이언트 (다른 터미널 = 다른 포트)
nc -u localhost 9090
# → "client-B" 입력

# 각 터미널에 자신이 보낸 메시지만 돌아와야 한다
```

자동화 테스트:

```bash
ctest -R UdpForward/MultiClientSessionIsolation --output-on-failure
```

**예상 출력**:

```
[ RUN      ] UdpForward.MultiClientSessionIsolation
[       OK ] UdpForward.MultiClientSessionIsolation
```

**검증**: `sessions_by_client_["ip:portA"]` → `target_fd_A`, `sessions_by_client_["ip:portB"]` → `target_fd_B` 로 분리되어 있어야 한다.

---

## 시나리오 3 — 세션 재사용

같은 소켓으로 패킷을 반복해서 보낼 때 매번 새 `target_fd`를 만들지 않고 기존 세션을 재사용하는지 확인한다.
재사용되지 않으면 fd가 계속 누적된다.

```bash
# 터미널 1 — 클라이언트 (같은 nc 세션에서 반복 입력)
nc -u localhost 9090
# → "round-0" 입력 → 응답 확인
# → "round-1" 입력 → 응답 확인
# → "round-2" 입력 → 응답 확인

# 프록시 PID 확인 후 fd 수가 늘어나지 않는지 확인
PROXY_PID=<pid>
ls /proc/$PROXY_PID/fd | wc -l   # 첫 메시지 후
ls /proc/$PROXY_PID/fd | wc -l   # 세 번째 메시지 후 — 동일해야 한다
```

자동화 테스트:

```bash
ctest -R UdpForward/SessionReuse --output-on-failure
```

**예상 출력**:

```
[ RUN      ] UdpForward.SessionReuse
[       OK ] UdpForward.SessionReuse
```

**검증**: `get_or_create_session()`이 기존 키를 찾아 `target_fd`를 재사용하면 fd 수가 일정하게 유지된다.

---

## 시나리오 4 — 세션 타임아웃 자동 정리

일정 시간 패킷이 없으면 세션이 자동으로 정리되고, 이후 재연결이 성공하는지 확인한다.
정리되지 않으면 클라이언트가 이탈해도 `target_fd`가 계속 쌓여 fd 고갈이 발생한다.

```bash
# 프록시 시작 후 초기 fd 수 기록
PROXY_PID=<pid>
ls /proc/$PROXY_PID/fd | wc -l

# 패킷 전송 → 세션 생성됨
echo "ping" | nc -u -q1 localhost 9090

# fd 수 증가 확인 (target_fd 1개 추가)
ls /proc/$PROXY_PID/fd | wc -l

# 타임아웃 대기 (기본 30초 + cleanup 10초 = 최대 40초)
sleep 41

# fd 수가 초기값으로 돌아왔는지 확인
ls /proc/$PROXY_PID/fd | wc -l

# 재연결 — 새 세션이 생성되어야 한다
echo "pong" | nc -u -q1 localhost 9090
```

자동화 테스트 (session_timeout=1초, cleanup_interval=1초로 단축):

```bash
ctest -R "UdpForward.SessionTimeout" --output-on-failure
```

**예상 출력**:

```
[ RUN      ] UdpForward.SessionTimeout
[       OK ] UdpForward.SessionTimeout
```

**검증**: `cleanup_expired_sessions()`가 호출되어 `sessions_by_client_` + `sessions_by_target_` + `target_fd`가 모두 정리된 뒤, 같은 클라이언트 주소로 새 세션이 생성되면 성공.

---

## 전체 Phase 5 테스트 한 번에

```bash
cd ~/tunnel-proxy/build
ctest -L phase5 --output-on-failure
```

**예상 출력**:

```
[==========] Running 4 tests from 1 test suite.
...
[==========] 4 tests passed.

100% tests passed, 0 tests failed out of 4
Total Test time (real) = X.XXs
```
