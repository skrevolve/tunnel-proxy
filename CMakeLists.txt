cmake_minimum_required(VERSION 3.14)
project(TunnelProxy VERSION 0.1.0 LANGUAGES CXX)

# C++17 설정
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 컴파일 옵션
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic")
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -fsanitize=address,undefined")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -march=native")

# 헤더 파일 경로
include_directories(${PROJECT_SOURCE_DIR}/include)

# OpenSSL 찾기 (Phase 4에서 사용)
find_package(OpenSSL REQUIRED)

# 외부 라이브러리 (FetchContent)
include(FetchContent)

# nlohmann/json v3.11.3
FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(nlohmann_json)

# Google Test v1.14.0
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# 소스 파일 목록
set(SOURCES
    src/main.cpp
    src/basic_proxy.cpp
    src/epoll_proxy.cpp
    src/utils/config.cpp
    src/utils/logger.cpp
)

# 실행 파일 생성
add_executable(proxy ${SOURCES})

# 라이브러리 링크
target_link_libraries(proxy
    OpenSSL::SSL
    OpenSSL::Crypto
    pthread
    nlohmann_json::nlohmann_json
)

# 테스트
enable_testing()
add_subdirectory(tests)

# 빌드 정보 출력
message(STATUS "========================================")
message(STATUS "Project: ${PROJECT_NAME} v${PROJECT_VERSION}")
message(STATUS "C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "Compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "========================================")
