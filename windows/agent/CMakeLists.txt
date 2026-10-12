add_executable(tethercam-agent WIN32 main.cpp)
target_link_libraries(tethercam-agent PRIVATE tcam_common WIL::WIL)
