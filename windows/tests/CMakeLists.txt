FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
    GIT_SHALLOW TRUE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(tcam_tests version_test.cpp)
target_link_libraries(tcam_tests PRIVATE tcam_common GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(tcam_tests)
