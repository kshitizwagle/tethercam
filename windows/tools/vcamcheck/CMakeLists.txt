add_executable(vcamcheck main.cpp)
target_link_libraries(vcamcheck PRIVATE tcam_common tcam_media WIL::WIL mfplat mf mfreadwrite mfuuid)
