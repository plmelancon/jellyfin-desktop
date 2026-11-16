include(GNUInstallDirs)

find_package(X11)
if(X11_FOUND AND X11_Xrandr_FOUND)
  include_directories(${X11_X11_INCLUDE_PATH} ${X11_Xrandr_INCLUDE_PATH})
  set(X11XRANDR_FOUND 1)
  add_definitions(-DUSE_X11XRANDR)
else()
  set(X11_LIBRARIES "")
  set(X11_Xrandr_LIB "")
endif()

# Display mode switching on KDE Plasma (Wayland), through KWin's output
# management protocols. Independent of X11; needs wayland-client and
# wayland-scanner.
if(PKG_CONFIG_FOUND)
  pkg_check_modules(WAYLAND_CLIENT QUIET IMPORTED_TARGET wayland-client)
endif()
find_program(WAYLAND_SCANNER_EXECUTABLE NAMES wayland-scanner)
if(WAYLAND_CLIENT_FOUND AND WAYLAND_SCANNER_EXECUTABLE)
  set(KWIN_OUTPUT_MANAGEMENT_FOUND 1)
  add_definitions(-DUSE_KWIN_OUTPUT_MANAGEMENT)
  message(STATUS "Enabling KDE Wayland display mode switching (KWin output management)")
elseif(REQUIRE_KWIN_OUTPUT_MANAGEMENT)
  message(FATAL_ERROR "REQUIRE_KWIN_OUTPUT_MANAGEMENT is set, but wayland-client or wayland-scanner was not found")
else()
  message(STATUS "Not building KDE Wayland display mode switching (wayland-client or wayland-scanner not found)")
endif()

if(LINUX_X11POWER)
  add_definitions(-DUSE_X11POWER)
  Message(STATUS "Enabling X11/XDG screensaver management")
else()
  add_definitions(-DLINUX_DBUS=1)
  Message(STATUS "Enabling D-Bus power management")
endif()

set(INSTALL_BIN_DIR ${CMAKE_INSTALL_BINDIR})
set(INSTALL_RESOURCE_DIR ${CMAKE_INSTALL_DATADIR}/jellyfin-desktop)

if(NOT OPENELEC)
  include(InstallLinuxDesktopFile)
endif()
