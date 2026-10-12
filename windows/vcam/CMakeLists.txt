add_library(tethercam-vcam SHARED dllmain.cpp tethercam-vcam.def)
target_link_libraries(tethercam-vcam PRIVATE tcam_common WIL::WIL)
