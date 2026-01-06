#!/bin/bash
# 테스트 스크립트

echo "Running tests..."

cd ../build

# 테스트 실행
ctest --output-on-failure

echo "Tests completed!"
