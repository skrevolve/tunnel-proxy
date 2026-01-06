#!/bin/bash
# 빌드 스크립트

set -e

BUILD_TYPE=${1:-Release}

echo "Building Tunnel Proxy (${BUILD_TYPE})..."

# 빌드 디렉토리 생성
mkdir -p ../build
cd ../build

# CMake 설정
cmake -DCMAKE_BUILD_TYPE=${BUILD_TYPE} ..

# 빌드
make -j$(nproc)

echo "Build completed!"
echo "Executable: ./proxy"
