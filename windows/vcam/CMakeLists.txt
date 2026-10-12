add_library(tethercam-vcam SHARED
    dllmain.cpp
    vcam.cpp
    frame_source.cpp
    tethercam-vcam.def)
target_link_libraries(tethercam-vcam PRIVATE tcam_common tcam_media WIL::WIL
    mfplat mfuuid mf runtimeobject advapi32)
# C4324: padding in WRL's module storage (SDK header).
target_compile_options(tethercam-vcam PRIVATE /wd4324)
