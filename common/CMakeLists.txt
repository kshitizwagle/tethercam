# Platform-neutral core shared by the Windows and Linux hosts: protocol codec, plist, usbmux client,
# sockets, formats, images, logging. Must not depend on COM or Media Foundation (see docs/DESIGN.md).
add_library(tcam_common STATIC
    src/version.cpp
    src/protocol.cpp
    src/plist.cpp
    src/net.cpp
    src/usbmux.cpp
    src/formats.cpp
    src/log.cpp
    src/annexb.cpp
    src/image.cpp
    src/agent_state.cpp
    src/status_server.cpp)
target_include_directories(tcam_common PUBLIC include)
target_compile_definitions(tcam_common PUBLIC TETHERCAM_VERSION="${PROJECT_VERSION}")
if(WIN32)
    # The shared-memory link to the FrameServer camera is Windows-only; Linux writes to v4l2loopback.
    target_sources(tcam_common PRIVATE src/shm.cpp)
    target_link_libraries(tcam_common PUBLIC ws2_32)
endif()
