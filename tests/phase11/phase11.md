# Phase 11 테스트 가이드

Phase 10까지는 기능 구현에 집중했다.
Phase 11은 **운영 안정성**을 검증한다: Heartbeat 타임아웃 감지, 지수 백오프 재연결, 메트릭 수집.
두 파일로 나뉜다: 메트릭 단위 테스트(TunnelMetrics)와 에이전트/서버 통합 테스트.

---

## 메트릭 구조

```
TunnelAgent / TunnelServer
  ↓ record_connection_attempt/success/failure()
  ↓ record_reconnect()
  ↓ record_bytes_sent/received()
  ↓ record_error()
TunnelMetrics (atomic 카운터)
  ↓ get_metrics().log_summary("agent")
Logger::info
```

---

## Phase 11-A — Heartbeat 타임아웃 감지

Phase 11-A 구현 내용 (`tunnel_agent.h`, `tunnel_server.h`):
- **TunnelAgent**: `last_ack_ns_` — HEARTBEAT_ACK 수신 시각 추적. `heartbeat_timeout_s_` 초과 시 fd shutdown → 재연결.
- **TunnelServer**: `AgentConn::last_heartbeat_ns` — HEARTBEAT 수신 시각 추적. watchdog 스레드가 `agent_timeout_s_` 초과 에이전트 fd shutdown.
- 단위 테스트 없음 (Phase 11-C 통합 테스트로 커버).

---

## Phase 11-B — 지수 백오프 재연결

Phase 11-B 구현 내용 (`tunnel_agent.cpp`):
- `run()` 재연결 루프: 1s → 2s → 4s → ... → 60s(max) 반복.
- `connect_and_run()`: 단일 연결 수명 처리 (running_ 변경 없음).
- `cleanup_connection()`: 세션/heartbeat/fd 정리 (멱등, cleanup_mutex_ 보호).
- `current_reconnect_delay()`: 현재 백오프 딜레이 외부 조회.
- 단위 테스트 없음 (Phase 11-C 통합 테스트 시나리오 14~15로 커버).

---

## Phase 11-C — 메트릭 수집 (`test_metrics.cpp`)

### 시나리오 1 — 초기 상태: 모든 카운터 0

새로 생성한 TunnelMetrics의 모든 카운터가 0이어야 한다.

```bash
ctest -R "TunnelMetricsBasic/InitialValues_AllZero" --output-on-failure
```

---

### 시나리오 2 — record_connection_attempt() → total_connection_attempts() 증가

```bash
ctest -R "TunnelMetricsBasic/RecordConnectionAttempt_Increments" --output-on-failure
```

**검증**: 2회 호출 → `total_connection_attempts() == 2`.

---

### 시나리오 3 — record_connection_success() → total_connection_successes() 증가

```bash
ctest -R "TunnelMetricsBasic/RecordConnectionSuccess_Increments" --output-on-failure
```

---

### 시나리오 4 — record_connection_failure() → total_connection_failures() 증가

```bash
ctest -R "TunnelMetricsBasic/RecordConnectionFailure_Increments" --output-on-failure
```

---

### 시나리오 5 — record_reconnect() → total_reconnects() 증가

```bash
ctest -R "TunnelMetricsBasic/RecordReconnect_Increments" --output-on-failure
```

---

### 시나리오 6 — record_bytes_sent(N) 여러 번 → total_bytes_sent() 누적합 일치

```bash
ctest -R "TunnelMetricsBasic/RecordBytesSent_Accumulates" --output-on-failure
```

**검증**: 100 + 200 + 300 = `total_bytes_sent() == 600`.

---

### 시나리오 7 — record_bytes_received(N) 여러 번 → total_bytes_received() 누적합 일치

```bash
ctest -R "TunnelMetricsBasic/RecordBytesReceived_Accumulates" --output-on-failure
```

**검증**: 512 + 512 = `total_bytes_received() == 1024`.

---

### 시나리오 8 — record_error() → total_errors() 증가

```bash
ctest -R "TunnelMetricsBasic/RecordError_Increments" --output-on-failure
```

---

### 시나리오 9 — attempts=0 → error_rate() == 0.0 (0 나누기 보호)

attempts 없이 error만 기록해도 error_rate()가 0.0을 반환해야 한다.

```bash
ctest -R "TunnelMetricsErrorRate/ZeroAttempts_ReturnsZero" --output-on-failure
```

---

### 시나리오 10 — attempts=10, errors=3 → error_rate() ≈ 0.3

```bash
ctest -R "TunnelMetricsErrorRate/ThreeErrorsOutOfTen_Returns0_3" --output-on-failure
```

**검증**: `error_rate() ≈ 0.3` (오차 1e-9 이내).

---

### 시나리오 11 — reset() → 모든 카운터 0 초기화

카운터를 채운 뒤 `reset()` 호출 후 모두 0이어야 한다.

```bash
ctest -R "TunnelMetricsReset/AfterReset_AllZero" --output-on-failure
```

---

### 시나리오 12 — 스레드 안전성: 동시 record_bytes_sent() 합계 정확

10개 스레드가 각각 1000번 `record_bytes_sent(100)` 호출 → `total_bytes_sent() == 1,000,000`.

```bash
ctest -R "TunnelMetricsThreadSafety/ConcurrentRecordBytes_ConsistentSum" --output-on-failure
```

**예상 출력**:
```
[ RUN      ] TunnelMetricsThreadSafety.ConcurrentRecordBytes_ConsistentSum
[       OK ] TunnelMetricsThreadSafety.ConcurrentRecordBytes_ConsistentSum
```

---

### 시나리오 13 — TunnelAgent 연결 성공 → connection_successes == 1

TunnelServer를 띄우고 TunnelAgent가 연결하면 `get_metrics().total_connection_successes() == 1`.

```bash
ctest -R "TunnelAgentMetrics/ConnectSuccess_RecordsSuccess" --output-on-failure
```

**검증**: `total_connection_successes() == 1`, `total_connection_attempts() >= 1`.

---

### 시나리오 14 — TunnelAgent 연결 실패 → connection_failures >= 1

없는 포트로 연결 시도 → `total_connection_failures() >= 1`.

```bash
ctest -R "TunnelAgentMetrics/ConnectFailure_RecordsFailure" --output-on-failure
```

**검증**: 2.5초 대기 후 `total_connection_failures() >= 1` (Phase 11-B 재연결 루프 확인).

---

### 시나리오 15 — TunnelAgent 재연결 발생 → reconnects >= 1

없는 서버로 연결 실패 후 1s 백오프 → 두 번째 시도(재연결) 기록.

```bash
ctest -R "TunnelAgentMetrics/Reconnect_RecordsReconnect" --output-on-failure
```

**검증**: 2초 대기 후 `total_reconnects() >= 1`.

---

### 시나리오 16 — TunnelServer 에이전트 등록 → connection_successes == 1

TunnelAgent가 HELLO_ACK까지 완료하면 서버 측 `total_connection_successes() == 1`.

```bash
ctest -R "TunnelServerMetrics/AgentRegister_RecordsSuccess" --output-on-failure
```

**예상 출력**:
```
[ RUN      ] TunnelServerMetrics.AgentRegister_RecordsSuccess
[       OK ] TunnelServerMetrics.AgentRegister_RecordsSuccess
```

---

### 시나리오 17 — TunnelServer 에이전트 해제 후 기록 유지

에이전트 연결 → 해제 → 서버 metrics에 연결 성공 기록이 유지되어야 한다.

```bash
ctest -R "TunnelServerMetrics/AgentDisconnect_RecordsEvent" --output-on-failure
```

**검증**: agent stop 후에도 `server.get_metrics().total_connection_successes() >= 1`.

---

## 전체 Phase 11 테스트 한 번에

```bash
cd ~/tunnel-proxy/build
ctest -L phase11 --output-on-failure
```

**예상 출력**:
```
100% tests passed, 0 tests failed out of 17
Total Test time (real) = X.XXs
```
