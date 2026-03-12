# Phase 6 테스트 가이드

Phase 5까지는 단순 포워딩이었다.
Phase 6은 **에이전트↔서버 간 바이너리 프레임이 올바르게 직렬화·파싱·분기되는가**를 검증한다.
멀티플렉싱의 기반인 16바이트 고정 헤더 포맷과 메시지 타입별 팩토리 함수가 핵심이다.

---

## 터널 프레임 구조

```
[ magic(4) | type(1) | flags(1) | reserved(2) | session_id(4) | length(4) ] + payload
   0x544E4C50                                   0=컨트롤 / 1+=세션
```

- `session_id == 0` : 컨트롤 채널 (HELLO / HEARTBEAT 등)
- `session_id >= 1` : 터널 세션 (OPEN / DATA / CLOSE 등)

---

## 시나리오 1 — 프레임 직렬화/파싱 왕복

DATA 프레임을 만들어 직렬화하고, 그 바이트 스트림을 역파싱했을 때 모든 필드가 원본과 일치하는지 확인한다.

```bash
ctest -R "TunnelProtocol/SerializeParseRoundtrip" --output-on-failure
```

**예상 출력**:

```
[ RUN      ] TunnelProtocol.SerializeParseRoundtrip
[       OK ] TunnelProtocol.SerializeParseRoundtrip
```

**검증**: `serialize()` 결과 크기 == `TUNNEL_HEADER_SIZE + payload.size()`,
역파싱 후 magic / type / flags / session_id / length / payload 전부 일치.

---

## 시나리오 2 — magic 불일치 → 예외

수신된 프레임의 magic 첫 바이트를 변조하면 `parse_header()`가 예외를 던져야 한다.
magic 검증이 없으면 엉뚱한 바이트 스트림도 그대로 처리되어 파싱 오류가 이후 로직으로 전파된다.

```bash
ctest -R "TunnelProtocol/InvalidMagic_Throws" --output-on-failure
```

**예상 출력**:

```
[ RUN      ] TunnelProtocol.InvalidMagic_Throws
[       OK ] TunnelProtocol.InvalidMagic_Throws
```

**검증**: `wire[0] ^= 0xFF` 변조 → `parse_header()` → `std::runtime_error`.

---

## 시나리오 3 — length 초과 → 예외

length 필드를 `TUNNEL_MAX_PAYLOAD + 1`로 조작하면 `parse_header()`가 예외를 던져야 한다.
상한 없이 받으면 메모리 할당 폭주(OOM)로 이어질 수 있다.

```bash
ctest -R "TunnelProtocol/LengthExceedsMax_Throws" --output-on-failure
```

**예상 출력**:

```
[ RUN      ] TunnelProtocol.LengthExceedsMax_Throws
[       OK ] TunnelProtocol.LengthExceedsMax_Throws
```

**검증**: 바이트 12-15 오버라이트 → `parse_header()` → `std::runtime_error`.

---

## 시나리오 4 — HELLO 페이로드 왕복

`make_hello("my-agent-001")` 로 만든 프레임의 payload를 `parse_hello_payload()`로 역파싱하면 agent_id가 그대로 나와야 한다.
HELLO는 에이전트가 서버에 자신을 알리는 첫 메시지이므로 agent_id 파싱 오류는 모든 라우팅을 망가뜨린다.

```bash
ctest -R "TunnelProtocol/HelloPayloadRoundtrip" --output-on-failure
```

**예상 출력**:

```
[ RUN      ] TunnelProtocol.HelloPayloadRoundtrip
[       OK ] TunnelProtocol.HelloPayloadRoundtrip
```

**검증**: `frame.type == HELLO`, `frame.session_id == 0`, `parse_hello_payload(frame.payload) == "my-agent-001"`.

---

## 시나리오 5 — OPEN 페이로드 왕복 (네트워크 바이트 오더)

`make_open(7, "192.168.1.10", 8080)` 페이로드를 `parse_open_payload()`로 역파싱하면 IP/포트가 일치해야 한다.
포트는 네트워크 바이트 오더(big-endian)로 저장되므로, 호스트 바이트 오더로 변환 없이 읽으면 포트가 뒤집힌다.

```bash
ctest -R "TunnelProtocol/OpenPayloadRoundtrip" --output-on-failure
```

**예상 출력**:

```
[ RUN      ] TunnelProtocol.OpenPayloadRoundtrip
[       OK ] TunnelProtocol.OpenPayloadRoundtrip
```

**검증**: payload 크기 6바이트 (IPv4 4 + 포트 2), `parse_open_payload()` → ip == `"192.168.1.10"`, port == 8080.

---

## 시나리오 6 — 팩토리 함수 type 필드 일관성

모든 팩토리 함수(`make_hello`, `make_hello_ack`, `make_open`, `make_open_ack`, `make_data`, `make_close`, `make_heartbeat`, `make_heartbeat_ack`)가 각자 올바른 `TunnelMsgType`을 반환하는지 확인한다.

```bash
ctest -R "TunnelProtocol/FactoryFunctions_CorrectType" --output-on-failure
```

**예상 출력**:

```
[ RUN      ] TunnelProtocol.FactoryFunctions_CorrectType
[       OK ] TunnelProtocol.FactoryFunctions_CorrectType
```

**검증**: 8개 팩토리 각각 `frame.type == <해당 TunnelMsgType>`.

---

## 시나리오 7 — OPEN 페이로드 크기 부족 → 예외

`parse_open_payload()`에 2바이트짜리 페이로드를 넘기면 예외를 던져야 한다.
최소 6바이트(IPv4 4 + 포트 2) 미만이면 파싱 자체가 불가능하다.

```bash
ctest -R "TunnelProtocol/OpenPayloadTooShort_Throws" --output-on-failure
```

**예상 출력**:

```
[ RUN      ] TunnelProtocol.OpenPayloadTooShort_Throws
[       OK ] TunnelProtocol.OpenPayloadTooShort_Throws
```

**검증**: `{0x01, 0x02}` 2바이트 페이로드 → `std::runtime_error`.

---

## 시나리오 8 — session_id 컨트롤/세션 채널 분리

컨트롤 메시지(HEARTBEAT, HEARTBEAT_ACK, HELLO, HELLO_ACK)는 항상 `session_id == 0`,
세션 메시지(CLOSE, OPEN_ACK)는 인수로 지정한 session_id를 그대로 반환해야 한다.
session_id가 0이 아닌 컨트롤 메시지는 라우팅 로직에서 잘못된 세션에 전달될 수 있다.

```bash
ctest -R "TunnelProtocol/ControlChannelSessionId" --output-on-failure
```

**예상 출력**:

```
[ RUN      ] TunnelProtocol.ControlChannelSessionId
[       OK ] TunnelProtocol.ControlChannelSessionId
```

**검증**: `make_heartbeat().session_id == 0`, `make_hello_ack().session_id == 0`,
`make_close(5).session_id == 5`, `make_open_ack(3).session_id == 3`.

---

## 전체 Phase 6 테스트 한 번에

```bash
cd ~/tunnel-proxy/build
ctest -L phase6 --output-on-failure
```

**예상 출력**:

```
[==========] Running 8 tests from 1 test suite.
...
[==========] 8 tests passed.

100% tests passed, 0 tests failed out of 8
Total Test time (real) = X.XXs
```
