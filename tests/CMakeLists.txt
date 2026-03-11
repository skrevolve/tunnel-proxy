include(GoogleTest)

# ─────────────────────────────────────────
# Phase 1 테스트
# ─────────────────────────────────────────
add_executable(test_phase1
    phase1/test_config.cpp
    phase1/test_logger.cpp
    ../src/utils/config.cpp
    ../src/utils/logger.cpp
)
target_include_directories(test_phase1 PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(test_phase1
    GTest::gtest_main
    nlohmann_json::nlohmann_json
)
gtest_discover_tests(test_phase1)

# ─────────────────────────────────────────
# Phase 2 테스트
# ─────────────────────────────────────────
add_executable(test_phase2
    phase2/test_epoll_proxy.cpp
    ../src/epoll_proxy.cpp
    ../src/utils/logger.cpp
)
target_include_directories(test_phase2 PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(test_phase2 GTest::gtest_main)
gtest_discover_tests(test_phase2)

# ─────────────────────────────────────────
# Phase 3 테스트 (zero-copy 벤치마크)
# ─────────────────────────────────────────
add_executable(test_phase3
    phase3/test_bench.cpp
    ../src/epoll_proxy.cpp
    ../src/basic_proxy.cpp
    ../src/utils/logger.cpp
)
target_include_directories(test_phase3 PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(test_phase3 GTest::gtest_main pthread)
gtest_discover_tests(test_phase3)

# ─────────────────────────────────────────
# Phase 4 테스트 (TLS 인증서 검증)
# ─────────────────────────────────────────
add_executable(test_phase4
    phase4/test_tls_verify.cpp
    ../src/tls_proxy.cpp
    ../src/utils/logger.cpp
)
target_include_directories(test_phase4 PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(test_phase4
    GTest::gtest_main
    OpenSSL::SSL
    OpenSSL::Crypto
)
gtest_discover_tests(test_phase4)

# ─────────────────────────────────────────
# Phase 5 테스트 (UDP 포워딩)
# ─────────────────────────────────────────
add_executable(test_phase5
    phase5/test_udp_forward.cpp
    ../src/udp_proxy.cpp
    ../src/utils/logger.cpp
)
target_include_directories(test_phase5 PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(test_phase5 GTest::gtest_main pthread)
gtest_discover_tests(test_phase5)
