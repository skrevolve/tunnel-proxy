---
name: check-build
description: tunnel-proxy 프로젝트를 Debug 모드로 빌드해 컴파일 에러를 확인한다. ctest는 실행하지 않는다 — CI가 대신한다.
disable-model-invocation: true
allowed-tools: Bash
---

# 빌드 확인

```bash
cd /home/skrevolve/tunneling-proxy/build && cmake -DCMAKE_BUILD_TYPE=Debug .. -Wno-dev && make -j$(nproc) 2>&1
```

## 성공 기준

마지막 출력이 아래와 같으면 성공:

```
[100%] Built target proxy
[100%] Built target test_phaseN
```

## 실패 시

에러 메시지를 읽고 수정 후 재빌드한다.
빌드 실패 상태로 커밋하지 않는다.
