# Google Test 찾기
find_package(GTest)

if(GTest_FOUND)
    # 테스트 실행 파일
    add_executable(test_logger test_logger.cpp ../src/utils/logger.cpp)
    target_link_libraries(test_logger GTest::GTest GTest::Main)
    add_test(NAME LoggerTest COMMAND test_logger)
    
    # TODO: 다른 테스트 추가
    # add_executable(test_config test_config.cpp ../src/utils/config.cpp)
    # target_link_libraries(test_config GTest::GTest GTest::Main)
    # add_test(NAME ConfigTest COMMAND test_config)
    
    message(STATUS "Tests enabled")
else()
    message(WARNING "GTest not found, tests disabled")
endif()
