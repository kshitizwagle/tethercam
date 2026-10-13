add_executable(tethercam-agent WIN32
    main.cpp
    engine.cpp
    install.cpp
    tray.cpp
    preview.cpp)
target_link_libraries(tethercam-agent PRIVATE tcam_common tcam_media WIL::WIL
    mfplat mfsensorgroup shell32 ole32 user32 gdi32)
