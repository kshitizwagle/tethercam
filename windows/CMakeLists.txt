cmake_minimum_required(VERSION 3.28)
set(TETHERCAM_BUILD_VERSION "0.1.0" CACHE STRING "Version stamped into the binaries and the MSI; CI sets it per build")
project(Tethercam VERSION ${TETHERCAM_BUILD_VERSION} LANGUAGES CXX RC)

if(NOT WIN32)
    message(FATAL_ERROR "The Tethercam Windows host only builds on Windows.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A00000C)
if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8 /Zc:__cplusplus)
endif()

include(FetchContent)
FetchContent_Declare(wil
    GIT_REPOSITORY https://github.com/microsoft/wil.git
    GIT_TAG v1.0.240803.1
    GIT_SHALLOW TRUE)
set(WIL_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WIL_BUILD_PACKAGING OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(wil)

option(TETHERCAM_BUILD_TESTS "Build unit tests" ON)

add_subdirectory(../common common)
add_subdirectory(media)
add_subdirectory(agent)
add_subdirectory(vcam)
add_subdirectory(tools/tcprobe)
add_subdirectory(tools/fakephone)
add_subdirectory(tools/vcamcheck)

if(TETHERCAM_BUILD_TESTS)
    enable_testing()
    add_subdirectory(../common/tests common-tests)
    add_subdirectory(tests)
endif()
