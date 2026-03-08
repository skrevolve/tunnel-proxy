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
