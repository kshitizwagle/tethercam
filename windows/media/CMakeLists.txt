# Media Foundation wrappers shared by the agent, vcam and tools.
add_library(tcam_media STATIC src/media.cpp)
target_include_directories(tcam_media PUBLIC include)
target_link_libraries(tcam_media
    PUBLIC tcam_common
    PRIVATE WIL::WIL mfplat mfuuid wmcodecdspuuid d3d11 dxgi dxguid gdi32 strmiids)
