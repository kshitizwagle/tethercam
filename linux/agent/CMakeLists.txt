find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavutil)

# FFmpeg decode/encode, shared by the agent and fakephone.
add_library(tcam_linux_media STATIC decoder.cpp encoder.cpp)
target_include_directories(tcam_linux_media PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tcam_linux_media PUBLIC tcam_common PkgConfig::FFMPEG)

add_library(tcam_linux_agent STATIC engine.cpp settings.cpp v4l2_output.cpp)
target_include_directories(tcam_linux_agent PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tcam_linux_agent PUBLIC tcam_common tcam_linux_media)

add_executable(tethercam-agent main.cpp)
target_link_libraries(tethercam-agent PRIVATE tcam_linux_agent)

option(TETHERCAM_TRAY "Tray icon via AyatanaAppIndicator (GTK 3)" ON)
if(TETHERCAM_TRAY)
    pkg_check_modules(TRAY IMPORTED_TARGET gtk+-3.0 ayatana-appindicator3-0.1)
    if(TRAY_FOUND)
        target_sources(tethercam-agent PRIVATE tray.cpp)
        target_compile_definitions(tethercam-agent PRIVATE TETHERCAM_TRAY)
        target_link_libraries(tethercam-agent PRIVATE PkgConfig::TRAY)
    else()
        message(WARNING "gtk+-3.0 / ayatana-appindicator3-0.1 not found: building without a tray icon")
    endif()
endif()

add_executable(fakephone ../../windows/tools/fakephone/main.cpp)
target_link_libraries(fakephone PRIVATE tcam_common tcam_linux_media)

install(TARGETS tethercam-agent RUNTIME DESTINATION bin)
