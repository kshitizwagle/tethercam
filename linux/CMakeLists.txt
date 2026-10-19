cmake_minimum_required(VERSION 3.28)
set(TETHERCAM_BUILD_VERSION "0.1.0" CACHE STRING "Version stamped into the binaries and packages; CI sets it per build")
project(Tethercam VERSION ${TETHERCAM_BUILD_VERSION} LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "The Tethercam Linux host only builds on Linux. Use windows/ on Windows.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
add_compile_options(-Wall -Wextra)

include(FetchContent)
option(TETHERCAM_BUILD_TESTS "Build unit tests" ON)

add_subdirectory(../common common)
add_subdirectory(agent)

if(TETHERCAM_BUILD_TESTS)
    enable_testing()
    add_subdirectory(../common/tests common-tests)
    add_subdirectory(tests)
endif()

# Packages: .deb and .rpm (cpack -G "DEB;RPM" in the build directory). The kernel module and usbmuxd
# come from the distro; the package configures and loads the module and enables the user service.
include(GNUInstallDirs)
set(CMAKE_INSTALL_DEFAULT_DIRECTORY_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
set(PKG ${CMAKE_CURRENT_SOURCE_DIR}/packaging)
install(FILES ${PKG}/tethercam-agent.service DESTINATION lib/systemd/user)
install(FILES ${PKG}/modprobe-tethercam.conf DESTINATION lib/modprobe.d RENAME tethercam.conf)
install(FILES ${PKG}/modules-load-tethercam.conf DESTINATION lib/modules-load.d RENAME tethercam.conf)
install(FILES ${PKG}/tethercam.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
# hicolor's index.theme stops at 512x512, so a lone 1024 icon is never found: ship the standard sizes
# (generated from altstore/icon.png).
set(TETHERCAM_ICON_SIZES 16 22 24 32 48 64 128 256 512)
set(_icon_dirs /usr/share/icons /usr/share/icons/hicolor)
foreach(size IN LISTS TETHERCAM_ICON_SIZES)
    install(FILES ${PKG}/icons/tethercam-${size}.png
            DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${size}x${size}/apps RENAME tethercam.png)
    list(APPEND _icon_dirs /usr/share/icons/hicolor/${size}x${size} /usr/share/icons/hicolor/${size}x${size}/apps)
endforeach()
install(FILES ../LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/tethercam)

# Debian wants the maintainer scripts under their exact names.
configure_file(${PKG}/postinst.sh ${CMAKE_CURRENT_BINARY_DIR}/deb/postinst COPYONLY)
configure_file(${PKG}/prerm.sh ${CMAKE_CURRENT_BINARY_DIR}/deb/prerm COPYONLY)

set(CPACK_PACKAGE_NAME tethercam)
set(CPACK_PACKAGE_VENDOR "Tethercam contributors")
set(CPACK_PACKAGE_CONTACT "Tethercam <https://github.com/kshitizwagle/tethercam/issues>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Use an iPhone as a webcam over USB")
set(CPACK_PACKAGE_DESCRIPTION "Tethercam streams an iPhone camera over USB at full resolution and exposes it as a
v4l2loopback webcam for browsers, Zoom, OBS and other apps. Video only, wired only, no overlays.")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/kshitizwagle/tethercam")
set(CPACK_PACKAGE_INSTALL_DIRECTORY /usr)
set(CPACK_PACKAGING_INSTALL_PREFIX /usr)
set(CPACK_RESOURCE_FILE_LICENSE ${CMAKE_CURRENT_SOURCE_DIR}/../LICENSE)
set(CPACK_STRIP_FILES ON)
# Each distro release links its own FFmpeg ABI, so CI builds one package per target and names it.
set(TETHERCAM_PACKAGE_TARGET "" CACHE STRING "Distro tag in package file names, e.g. ubuntu24.04 or fedora")
if(TETHERCAM_PACKAGE_TARGET)
    set(_tag "-${TETHERCAM_PACKAGE_TARGET}")
endif()
set(CPACK_PACKAGE_FILE_NAME "Tethercam-${PROJECT_VERSION}${_tag}-x86_64")

set(CPACK_DEBIAN_FILE_NAME "Tethercam-${PROJECT_VERSION}${_tag}-amd64.deb")
set(CPACK_DEBIAN_PACKAGE_SECTION video)
set(CPACK_DEBIAN_PACKAGE_LICENSE GPL-3.0)
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_DEPENDS "v4l2loopback-dkms | v4l2loopback-modules, usbmuxd, kmod")
# Wi-Fi discovery talks to avahi-daemon (libavahi-client comes in through shlibdeps).
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "avahi-daemon")
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "va-driver-all, gnome-shell-extension-appindicator | plasma-workspace")
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA "${CMAKE_CURRENT_BINARY_DIR}/deb/postinst;${CMAKE_CURRENT_BINARY_DIR}/deb/prerm")
set(CPACK_DEBIAN_PACKAGE_CONTROL_STRICT_PERMISSION ON)

set(CPACK_RPM_FILE_NAME "Tethercam-${PROJECT_VERSION}${_tag}-x86_64.rpm")
set(CPACK_RPM_PACKAGE_LICENSE GPL-3.0-or-later)
set(CPACK_RPM_PACKAGE_GROUP "Applications/Multimedia")
set(CPACK_RPM_PACKAGE_AUTOREQ ON)
# Fedora: v4l2loopback is in RPM Fusion (akmod), as is an FFmpeg with the H.264 decoder.
set(CPACK_RPM_PACKAGE_REQUIRES "v4l2loopback, usbmuxd, kmod")
set(CPACK_RPM_POST_INSTALL_SCRIPT_FILE ${PKG}/postinst.sh)
set(CPACK_RPM_PRE_UNINSTALL_SCRIPT_FILE ${PKG}/prerm.sh)
set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
    /usr/lib/systemd /usr/lib/systemd/user /usr/lib/modprobe.d /usr/lib/modules-load.d
    ${_icon_dirs} /usr/share/applications)

set(CPACK_GENERATOR "DEB;RPM")
include(CPack)
