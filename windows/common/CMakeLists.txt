# Core shared by the agent and tools: protocol codec, plist, usbmux client, sockets, shm layout.
# Must not depend on COM or Media Foundation (see docs/DESIGN.md).
add_library(tcam_common STATIC
    src/version.cpp
    src/protocol.cpp
    src/plist.cpp
    src/net.cpp
    src/usbmux.cpp
    src/formats.cpp
    src/log.cpp
    src/shm.cpp
    src/annexb.cpp
    src/image.cpp)
target_include_directories(tcam_common PUBLIC include)
target_compile_definitions(tcam_common PUBLIC TETHERCAM_VERSION="${PROJECT_VERSION}")
target_link_libraries(tcam_common PUBLIC ws2_32)
