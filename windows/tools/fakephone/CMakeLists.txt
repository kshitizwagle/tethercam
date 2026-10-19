add_executable(fakephone main.cpp)
target_link_libraries(fakephone PRIVATE tcam_common tcam_media dnsapi)
