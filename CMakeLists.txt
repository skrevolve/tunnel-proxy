cmake_minimum_required(VERSION 3.14)
project(TunnelProxy VERSION 0.1.0 LANGUAGES CXX)

# C++17 설정
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# _GNU_SOURCE: splice(), pipe2() 등 Linux 전용 시스템 콜 사용
add_compile_definitions(_GNU_SOURCE)

# 컴파일 옵션
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic")
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -fsanitize=address,undefined")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -march=native")

# 헤더 파일 경로
include_directories(${PROJECT_SOURCE_DIR}/include)

# OpenSSL 찾기 (Phase 4에서 사용)
find_package(OpenSSL REQUIRED)

# ZLIB 찾기 (Phase 8-B: PNG 인코딩에 사용)
find_package(ZLIB REQUIRED)

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

# FreeRDP 3.x (Phase 8-B: RDP 클라이언트 + 화면 스트리밍)
# ── 헤드리스 빌드 — 디스플레이/오디오/프린터 등 UI 관련 옵션 모두 비활성화 ──
# WITH_CLIENT_COMMON=OFF: fuse3 의존성 제거 (드라이브 리다이렉션 채널 미사용)
# WITH_BINARY_VERSIONING=OFF: CMake 타겟 이름을 freerdp/winpr로 고정 (버전 suffix 제거)
set(WITH_X11                OFF CACHE BOOL "" FORCE)
set(WITH_WAYLAND            OFF CACHE BOOL "" FORCE)
set(WITH_ALSA               OFF CACHE BOOL "" FORCE)
set(WITH_PULSE              OFF CACHE BOOL "" FORCE)
set(WITH_OSS                OFF CACHE BOOL "" FORCE)
set(WITH_CUPS               OFF CACHE BOOL "" FORCE)
set(WITH_PCSC               OFF CACHE BOOL "" FORCE)
set(WITH_FFMPEG             OFF CACHE BOOL "" FORCE)
set(WITH_SWSCALE            OFF CACHE BOOL "" FORCE)
set(WITH_GSSAPI             OFF CACHE BOOL "" FORCE)
set(WITH_KRB5               OFF CACHE BOOL "" FORCE)
set(WITH_CAIRO              OFF CACHE BOOL "" FORCE)
set(WITH_JPEG               OFF CACHE BOOL "" FORCE)
set(WITH_OPENH264           OFF CACHE BOOL "" FORCE)
set(WITH_LAME               OFF CACHE BOOL "" FORCE)
set(WITH_OPUS               OFF CACHE BOOL "" FORCE)
set(WITH_FAAC               OFF CACHE BOOL "" FORCE)
set(WITH_FAAD2              OFF CACHE BOOL "" FORCE)
set(WITH_GSM                OFF CACHE BOOL "" FORCE)
set(WITH_WEBP               OFF CACHE BOOL "" FORCE)
set(WITH_DSP_FFMPEG         OFF CACHE BOOL "" FORCE)
set(WITH_SAMPLE             OFF CACHE BOOL "" FORCE)
set(WITH_SHADOW             OFF CACHE BOOL "" FORCE)
set(WITH_PROXY              OFF CACHE BOOL "" FORCE)
set(WITH_SERVER             OFF CACHE BOOL "" FORCE)
set(WITH_CHANNELS           OFF CACHE BOOL "" FORCE)
set(WITH_CLIENT             OFF CACHE BOOL "" FORCE)
set(WITH_CLIENT_COMMON      OFF CACHE BOOL "" FORCE)
set(WITH_FUSE               OFF CACHE BOOL "" FORCE)
set(WITH_BINARY_VERSIONING  OFF CACHE BOOL "" FORCE)
set(FREERDP_BUILD_TESTING   OFF CACHE BOOL "" FORCE)
set(FREERDP_BUILD_EXAMPLES  OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS       OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    FreeRDP
    URL https://github.com/FreeRDP/FreeRDP/archive/refs/tags/3.6.2.tar.gz
)
FetchContent_MakeAvailable(FreeRDP)

# libssh2 1.11.1 (Phase 8-C: SSH 클라이언트)
# 1.11.0에는 FetchContent 호환 버그(CMAKE_SOURCE_DIR vs CMAKE_CURRENT_SOURCE_DIR)가 있어 1.11.1 사용
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING     OFF CACHE BOOL "" FORCE)
set(LIBSSH2_BUILD_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    libssh2
    URL https://github.com/libssh2/libssh2/releases/download/libssh2-1.11.1/libssh2-1.11.1.tar.gz
)
FetchContent_MakeAvailable(libssh2)

# LibVNCServer 0.9.14 (Phase 8-D: VNC 클라이언트)
# ── SASL은 optional — 시스템에 없으면 자동 스킵 ──
set(WITH_JPEG         OFF CACHE BOOL "" FORCE)
set(WITH_PNG          OFF CACHE BOOL "" FORCE)
set(WITH_GCRYPT       OFF CACHE BOOL "" FORCE)
set(WITH_GNUTLS       OFF CACHE BOOL "" FORCE)
set(WITH_SDL          OFF CACHE BOOL "" FORCE)
set(WITH_GTK          OFF CACHE BOOL "" FORCE)
set(WITH_FFMPEG       OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    libvncserver
    URL https://github.com/LibVNC/libvncserver/archive/refs/tags/LibVNCServer-0.9.14.tar.gz
)
FetchContent_MakeAvailable(libvncserver)

# 소스 파일 목록
set(SOURCES
    src/main.cpp
    src/basic_proxy.cpp
    src/epoll_proxy.cpp
    src/tls_proxy.cpp
    src/udp_proxy.cpp
    src/tunnel_protocol.cpp
    src/tunnel_agent.cpp
    src/tunnel_server.cpp
    src/mtls_context.cpp
    src/jwt_verifier.cpp
    src/access_policy.cpp
    src/guac_parser.cpp
    src/guac_rdp.cpp
    src/guac_ssh.cpp
    src/guac_vnc.cpp
    src/guac_websocket.cpp
    src/utils/config.cpp
    src/utils/logger.cpp
)

# 실행 파일 생성
add_executable(proxy ${SOURCES})

# FreeRDP include 경로 설정
# FreeRDP은 target_include_directories(PUBLIC ...) 대신 include_directories()를 사용해
# FetchContent 시 include 경로가 자동으로 전파되지 않는다. 직접 추가 필요.
target_include_directories(proxy SYSTEM PRIVATE
    ${freerdp_SOURCE_DIR}/include
    ${freerdp_BINARY_DIR}/include
    ${freerdp_SOURCE_DIR}/winpr/include
    ${freerdp_BINARY_DIR}/winpr/include
    ${libssh2_SOURCE_DIR}/include
    ${libvncserver_SOURCE_DIR}
    ${libvncserver_BINARY_DIR}
)

# 라이브러리 링크
target_link_libraries(proxy
    freerdp          # FreeRDP 코어 + GDI (Phase 8-B)
    winpr            # WinPR — FreeRDP의 Windows 이식성 레이어
    libssh2_static   # SSH 클라이언트 (Phase 8-C)
    vncclient        # VNC 클라이언트 (Phase 8-D)
    ZLIB::ZLIB       # PNG 인코딩 (Phase 8-B/D)
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
