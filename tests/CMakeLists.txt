# Google Test 찾기
find_package(GTest)

if(GTest_FOUND)
    add_executable(test_logger test_logger.cpp ../src/utils/logger.cpp)
    target_include_directories(test_logger PRIVATE ${PROJECT_SOURCE_DIR}/include)
    target_link_libraries(test_logger GTest::GTest GTest::Main)
    add_test(NAME LoggerTest COMMAND test_logger)

    message(STATUS "Tests enabled")
else()
    message(WARNING "GTest not found, tests disabled")
endif()
