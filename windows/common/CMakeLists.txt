# Platform-neutral core: protocol codec, plist, usbmux framing, shm layout.
# Must not depend on COM or Media Foundation (see DESIGN.md).
add_library(tcam_common STATIC
    src/version.cpp)
target_include_directories(tcam_common PUBLIC include)
target_compile_definitions(tcam_common PUBLIC TETHERCAM_VERSION="${PROJECT_VERSION}")
