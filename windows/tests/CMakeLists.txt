# GoogleTest comes from common/tests (added first by windows/CMakeLists.txt).
add_executable(tcam_agent_tests quality_engine_test.cpp wifi_engine_test.cpp ../agent/engine.cpp ../agent/discovery.cpp)
target_link_libraries(tcam_agent_tests PRIVATE tcam_common tcam_media GTest::gtest_main avrt dnsapi)
include(GoogleTest)
gtest_discover_tests(tcam_agent_tests)
