---
name: new-phase
description: tunnel-proxy 프로젝트에서 새 Phase 세션을 시작한다. master pull → 브랜치 생성까지 수행. Phase 마지막 세션(테스트 포함)이면 시나리오를 먼저 제안하고 사람의 확인을 기다린다.
disable-model-invocation: true
argument-hint: <phase-id> <branch-name> [last]
---

# 새 Phase 세션 시작

인자: `$ARGUMENTS`
- 첫 번째: phase ID (예: `6-A`, `6-B`)
- 두 번째: 브랜치명 (예: `feat/tunnel-protocol`)
- 세 번째(선택): `last` — Phase 마지막 세션(테스트 포함)임을 명시

## 실행 순서

### 1. master 최신화 + 브랜치 생성

```bash
git checkout master && git pull origin master
git checkout -b <브랜치명>
```

### 2. Phase 마지막 세션 여부 확인

세 번째 인자가 `last`이거나 브랜치명이 `test/`로 시작하면 **Phase 마지막 세션**이다.

**Phase 마지막 세션인 경우 — 코드를 한 줄도 작성하기 전에:**

1. 이번 Phase에서 검증할 시나리오 초안을 작성한 뒤, 공개 API 함수 목록과 대조해 각 함수의 성공/실패 경로(팩토리·로더 포함)가 모두 커버되었는지 확인하고 누락을 채운다.
2. 완성된 시나리오 목록을 번호 붙여 제안한다.
3. 사람이 확인(ㅇㅇ / 네 / 수정 요청)할 때까지 테스트 코드 작성을 시작하지 않는다.
3. 확인 후 → 테스트 코드 작성 → 사람 재확인 → 구현 진행.

**일반 세션인 경우:**

CLAUDE.md의 해당 Phase 작업 내용을 확인하고 구현을 시작한다.

## 시나리오 제안 형식 (마지막 세션 전용)

```
## [Phase X] 테스트 시나리오

**[그룹명]**
1. [검증 내용]
2. [검증 내용]

**[그룹명]**
3. [검증 내용]

이 시나리오들이 원하시는 검증 범위와 맞는가?
```

## 주의사항

- 브랜치는 항상 최신 master 기준으로 생성한다.
- 다음 세션 범위의 TODO는 주석만 남기고 절대 구현하지 않는다.
- PR 생성 = 세션 종료. PR 이후 다음 Phase 작업 시작 금지.
