# @brief Engine Unit Tests
# @details Defines the test executable and discovers GoogleTest cases.

add_executable(VortexTests test_main.cpp)

target_compile_features(VortexTests PUBLIC cxx_std_20)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set_target_properties(VortexTests PROPERTIES CXX_SCAN_FOR_MODULES ON)
endif()

target_link_libraries(VortexTests 
    PRIVATE 
        GTest::gtest_main
        benchmark::benchmark
        VortexCore
)

include(GoogleTest)
gtest_discover_tests(VortexTests)