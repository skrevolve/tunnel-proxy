---
name: finish-phase
description: tunnel-proxy 프로젝트의 Phase 작업을 마무리한다. 커밋 → push → CI 확인 → CLAUDE.md 템플릿대로 PR 생성.
disable-model-invocation: true
allowed-tools: Bash
---

# Phase 마무리 및 PR 생성

## 실행 순서

### 1. 커밋

```bash
git add <변경된 파일들>
git commit -m "<type>(<scope>): <description>

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>"
```

커밋 타입: `feat` / `fix` / `perf` / `refactor` / `test` / `docs` / `build`

### 2. Push

```bash
git push origin <브랜치명>
```

### 3. CI 확인

push 후 약 1분 대기:

```bash
gh run list --limit 1
```

- `completed success` → PR 생성으로 진행
- `completed failure` → 에러 확인 후 수정, 재push

### 4. PR 생성

아래 템플릿을 반드시 지킨다. 처음 보는 사람도 코드를 읽지 않고 PR만으로 완전히 이해할 수 있을 정도로 상세하게 작성한다.

```
gh pr create --title "<커밋 제목>" --body "$(cat <<'EOF'
## 작업 배경
이전 단계의 어떤 구조적 한계 때문에 이 작업이 필요해졌는가.
추상적인 표현 금지. 숫자, 시나리오, 구체적인 상황으로 설명.

## 핵심 개념
이 작업에서 처음 등장하는 기술/시스템 콜/자료구조를 한 줄씩 설명.
- 개념: 한 줄 본질 요약

## 변경 사항
파일별로 무엇을 추가/수정했는가. 함수 단위로 기록.
- 파일명
  - 함수(): 설명

## 핵심 구현 결정
| 코드 / 선택 | 이유 | 다른 방법을 안 쓴 이유 |
|-------------|------|------------------------|
| 선택 | 이유 | 대안을 쓰지 않은 이유 |

## 빌드 결과
\`\`\`
[100%] Built target proxy
\`\`\`

## CI 결과
\`\`\`
completed  success  ...
\`\`\`

## 이 작업으로 가능해진 것 / 다음 단계
- 이전: 상태
- 이후: 상태
- 다음 세션: Phase X-Y 작업
EOF
)" --base master
```

## 주의사항

- PR 생성 = 세션 종료. PR 이후 다음 Phase 작업을 이어서 시작하지 않는다.
- 머지는 절대 하지 않는다. 사람이 직접 한다.
