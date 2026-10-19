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
    src/status_server.cpp
    src/noise.cpp
    src/link.cpp
    src/pairing.cpp)
target_include_directories(tcam_common PUBLIC include)

# Monocypher 4.0.2 (BSD-2-Clause / CC0), vendored unmodified: X25519, BLAKE2b and ChaCha20-Poly1305 for
# the Wi-Fi transport. The iOS app compiles the same files.
enable_language(C)
add_library(monocypher STATIC ../third_party/monocypher/monocypher.c)
target_include_directories(monocypher PUBLIC ../third_party/monocypher)
set_target_properties(monocypher PROPERTIES C_STANDARD 99 POSITION_INDEPENDENT_CODE ON)
target_compile_options(monocypher PRIVATE $<IF:$<C_COMPILER_ID:MSVC>,/w,-w>)  # vendored, kept unmodified
target_link_libraries(tcam_common PRIVATE monocypher)
target_compile_definitions(tcam_common PUBLIC TETHERCAM_VERSION="${PROJECT_VERSION}")
if(WIN32)
    # The shared-memory link to the FrameServer camera is Windows-only; Linux writes to v4l2loopback.
    target_sources(tcam_common PRIVATE src/shm.cpp)
    target_link_libraries(tcam_common PUBLIC ws2_32 bcrypt crypt32)
endif()
