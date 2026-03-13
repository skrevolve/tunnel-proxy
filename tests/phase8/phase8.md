# Phase 8 테스트 가이드

Phase 7까지는 터널 인증/정책을 검증했다.
Phase 8은 **브라우저에서 원격 데스크톱/터미널에 접근**하는 Guacamole 프로토콜 스택을 검증한다.
두 파일로 나뉜다: 프로토콜 직렬화 단위(GuacParser)와 실제 프로토콜 통신(VNC/SSH/RDP/WebSocket).

---

## Guacamole 프로토콜 구조

```
브라우저 (WebSocket)
  ↓  RFC 6455 WebSocket 프레임
[1] GuacWebSocketGateway — 연결 수락 + 프로토콜 라우팅
  ↓  connect "rdp"/"ssh"/"vnc"
[2] GuacRdpClient / GuacSshClient / GuacVncClient
  ↓  실제 프로토콜 연결 (FreeRDP / libssh2 / libvncclient)
[3] 원격 서버 (RDP 3389 / SSH 22 / VNC 5900)
  ↓  화면/터미널 데이터
[4] Guacamole instruction 직렬화 (GuacParser::serialize)
  ↓  img/blob/end, size/pipe/blob
브라우저로 전달
```

---

## Phase 8-A — GuacParser (`test_guac_parser.cpp`)

### 시나리오 1 — 완전한 명령어 한 개 파싱

와이어 포맷 `"3.img,1.0,4.Over;"` → opcode=`"img"`, args=`["0","Over"]` 파싱 확인.

```bash
ctest -R "GuacParserParsing/CompleteInstruction_ParsesOpcodeAndArgs" --output-on-failure
```

**예상 출력**:
```
[ RUN      ] GuacParserParsing.CompleteInstruction_ParsesOpcodeAndArgs
[       OK ] GuacParserParsing.CompleteInstruction_ParsesOpcodeAndArgs
```

**검증**: `instr.opcode == "img"`, `instr.args == ["0","Over"]`, `has_instruction() == false` (소비 후).

---

### 시나리오 2 — 한 번의 feed()에 여러 명령어 공급

`"3.img;3.end;"` → 큐에 두 개 순서대로 대기, 순서대로 꺼낼 수 있어야 한다.

```bash
ctest -R "GuacParserParsing/MultipleInstructionsInOneFeed_AllQueued" --output-on-failure
```

**검증**: `next_instruction().opcode == "img"` → `"end"` 순서, 이후 `has_instruction() == false`.

---

### 시나리오 3 — 청크 분할 공급

TCP 스트리밍 환경에서 `"3.im"` + `"g;"` 두 번의 feed()로 하나의 명령어를 복원해야 한다.

```bash
ctest -R "GuacParserParsing/ChunkedFeed_ReconstructsInstruction" --output-on-failure
```

**검증**: 첫 feed → `has_instruction() == false`, 두 번째 feed 후 → `opcode == "img"`.

---

### 시나리오 4 — 빈 arg

`"3.foo,0.,3.bar;"` → `args[0] == ""`, `args[1] == "bar"`. `0.` = 길이 0인 element.

```bash
ctest -R "GuacParserParsing/EmptyArg_ParsedAsEmptyString" --output-on-failure
```

---

### 시나리오 5 — 공급 전후 has_instruction()

빈 파서는 `has_instruction() == false`, 완전한 명령어 공급 후 `true`.

```bash
ctest -R "GuacParserParsing/HasInstruction_FalseBeforeFeedTrueAfter" --output-on-failure
```

---

### 시나리오 6 — length 필드에 비숫자 → runtime_error

`"!.img;"` → length 자리에 비숫자 → 즉시 `std::runtime_error`.

```bash
ctest -R "GuacParserError/NonDigitInLengthField_Throws" --output-on-failure
```

---

### 시나리오 7 — 구분자 자리에 비구분자 → runtime_error

`"3.img|"` → element 읽기 완료 후 `,`나 `;` 대신 다른 문자 → `std::runtime_error`.

```bash
ctest -R "GuacParserError/InvalidSeparator_Throws" --output-on-failure
```

---

### 시나리오 8 — length 없이 '.' 시작 → runtime_error

`".img;"` → length 필드 비어있는 상태에서 `.` → `std::runtime_error`.

```bash
ctest -R "GuacParserError/EmptyLengthBeforeDot_Throws" --output-on-failure
```

---

### 시나리오 9 — serialize() 와이어 포맷 정확성

`{opcode="img", args=["0","Over"]}` → `"3.img,1.0,4.Over;"`.

```bash
ctest -R "GuacParserSerialize/BasicInstruction_ProducesWireFormat" --output-on-failure
```

---

### 시나리오 10 — 빈 opcode → invalid_argument

opcode 없이 직렬화 시도 → 즉시 `std::invalid_argument`.

```bash
ctest -R "GuacParserSerialize/EmptyOpcode_ThrowsInvalidArgument" --output-on-failure
```

---

### 시나리오 11 — 직렬화 → feed() 왕복

`serialize()` 결과를 다시 `feed()`하면 원본 instruction이 복원되어야 한다.

```bash
ctest -R "GuacParserSerialize/RoundTrip_FeedAfterSerialize_RestoresOriginal" --output-on-failure
```

**검증**: `restored.opcode == original.opcode`, `restored.args == original.args`.

---

### 시나리오 12 — reset() 큐 + 상태 초기화

완성 큐에 있는 명령어와 파싱 중인 중간 상태를 `reset()` 후 모두 비워야 한다.

```bash
ctest -R "GuacParserReset/ResetClearsStateAndQueue" --output-on-failure
```

**검증**: `"3.img;3.en"` 공급 → `has_instruction() == true` → `reset()` → `false`, 이후 `"3.end;"` 정상 파싱.

---

## Phase 8-B/C/D — 프로토콜 통신 (`test_guac_protocol.cpp`)

### 시나리오 13 — mock VNC 서버 연결 → size instruction 수신

MockVncServer(RFB 3.8, Raw 64×64)에 연결하면 ServerInit 수신 후 `size` instruction이 콜백으로 전달되어야 한다.

```bash
ctest -R "GuacVncClientProtocol/ConnectMockServer_ReceivesSizeInstruction" --output-on-failure
```

**예상 출력**:
```
[ RUN      ] GuacVncClientProtocol.ConnectMockServer_ReceivesSizeInstruction
[       OK ] GuacVncClientProtocol.ConnectMockServer_ReceivesSizeInstruction
```

**검증**: `size` instruction의 `args[1] == "64"`, `args[2] == "64"`.

---

### 시나리오 14 — FramebufferUpdate → img/blob/end 시퀀스

mock 서버가 Raw encoding FramebufferUpdate를 보내면 `img → blob... → end` GuacInstruction 시퀀스가 전달되어야 한다.

```bash
ctest -R "GuacVncClientProtocol/FramebufferUpdate_ReceivesImgBlobEndSequence" --output-on-failure
```

**검증**: `wait_for_opcode("img")`, `wait_for_opcode("blob")`, `wait_for_opcode("end")` 모두 5초 이내 수신.

---

### 시나리오 15 — disconnect() 후 is_connected() false

`disconnect()` 호출 후 반드시 `is_connected() == false`.

```bash
ctest -R "GuacVncClientProtocol/Disconnect_IsConnectedFalse" --output-on-failure
```

---

### 시나리오 16 — SSH 연결 → size + pipe instruction 수신

loopback sshd(port 22)에 연결하면 `size` → `pipe` 순서로 instruction이 전달되어야 한다.
sshd 미가용 시 GTEST_SKIP.

```bash
ctest -R "GuacSshClientProtocol/ConnectLocalSshd_ReceivesSizeAndPipe" --output-on-failure
```

**검증**: `size` instruction → `pipe` instruction 수신. `pipe`의 args[0]은 스트림 ID.

---

### 시나리오 17 — SSH 터미널 입력 전달 → blob 수신

`send_input("echo hello\n")`를 호출하면 셸 에코가 `blob` instruction으로 돌아와야 한다.
sshd 미가용 시 GTEST_SKIP.

```bash
ctest -R "GuacSshClientProtocol/SendInput_ReceivesBlobResponse" --output-on-failure
```

**검증**: `send_input("echo hello\n")` → 3초 이내 `blob` instruction 수신.

---

### 시나리오 18 — SSH disconnect() 후 end instruction

`disconnect()` 후 세션 종료를 알리는 `end` instruction이 전달되어야 한다.
sshd 미가용 시 GTEST_SKIP.

```bash
ctest -R "GuacSshClientProtocol/Disconnect_ReceivesEndInstruction" --output-on-failure
```

---

### 시나리오 19 — RDP 연결 거부 → is_connected() false (크래시 없음)

리슨 중인 서버가 없는 포트(13389)로 연결을 시도하면 FreeRDP가 실패를 처리하고 `connected_ == false`를 유지해야 한다.

```bash
ctest -R "GuacRdpClientProtocol/ConnectionRefused_IsConnectedFalse" --output-on-failure
```

**예상 출력**:
```
[ RUN      ] GuacRdpClientProtocol.ConnectionRefused_IsConnectedFalse
[       OK ] GuacRdpClientProtocol.ConnectionRefused_IsConnectedFalse
```

**검증**: `connect()` 후 3초 대기 → `is_connected() == false`, `disconnect()` 안전 호출.

---

## Phase 8-E — WebSocket 게이트웨이 (`test_guac_protocol.cpp`)

### 시나리오 20 — start() 후 is_running() true

`start(port)` 호출 후 즉시 `is_running() == true`.

```bash
ctest -R "GuacWebSocketLifecycle/StartSetsRunning" --output-on-failure
```

---

### 시나리오 21 — stop() 후 is_running() false

`stop()` 호출 후 `is_running() == false`.

```bash
ctest -R "GuacWebSocketLifecycle/StopClearsRunning" --output-on-failure
```

---

### 시나리오 22 — stop() 후 재 start() 정상 동작

`stop()` 후 새 포트로 `start()` 재호출이 정상 동작해야 한다.

```bash
ctest -R "GuacWebSocketLifecycle/RestartAfterStop_Works" --output-on-failure
```

---

### 시나리오 23 — WebSocket 핸드셰이크 → HTTP 101

일반 TCP 클라이언트로 HTTP Upgrade 요청을 보내면 `"101 Switching Protocols"` 응답을 받아야 한다.

```bash
ctest -R "GuacWebSocketProtocol/Handshake_Returns101SwitchingProtocols" --output-on-failure
```

**예상 출력**:
```
[ RUN      ] GuacWebSocketProtocol.Handshake_Returns101SwitchingProtocols
[       OK ] GuacWebSocketProtocol.Handshake_Returns101SwitchingProtocols
```

**검증**: HTTP 응답에 `"101 Switching Protocols"` + `"Upgrade: websocket"` 포함.

---

### 시나리오 24 — WebSocket 프레임 송수신

WebSocket 핸드셰이크 후 텍스트 프레임을 보내고 응답을 받을 수 있어야 한다.
`connect` instruction 전송 → 게이트웨이가 error 또는 다른 instruction으로 응답.

```bash
ctest -R "GuacWebSocketProtocol/SendFrame_ReceivesResponse" --output-on-failure
```

**검증**: masked 텍스트 프레임 전송 → 5초 이내 응답 프레임 수신.

---

### 시나리오 25 — 미지원 프로토콜 → error instruction

`connect` instruction의 프로토콜이 "rdp"/"ssh"/"vnc" 외의 값이면 `error` GuacInstruction을 받아야 한다.

```bash
ctest -R "GuacWebSocketProtocol/ConnectUnknownProtocol_ReturnsErrorInstruction" --output-on-failure
```

**예상 출력**:
```
[ RUN      ] GuacWebSocketProtocol.ConnectUnknownProtocol_ReturnsErrorInstruction
[       OK ] GuacWebSocketProtocol.ConnectUnknownProtocol_ReturnsErrorInstruction
```

**검증**: `"connect unknown_protocol"` → 응답에 `"error"` opcode 포함.

---

## 전체 Phase 8 테스트 한 번에

```bash
cd ~/tunnel-proxy/build
ctest -L phase8 --output-on-failure
```

**예상 출력**:
```
100% tests passed, 0 tests failed out of 25
Total Test time (real) = X.XXs
```

> SSH 테스트(시나리오 16~18)는 CI에서 openssh-server 설치 + runner 계정 설정 후 실행된다.
> 로컬에서 sshd가 없으면 해당 테스트는 `SKIPPED`로 표시된다.
