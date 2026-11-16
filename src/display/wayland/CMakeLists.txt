# Client code for KDE's output device and output management protocols,
# generated from the XML files in protocols/. These are copied unchanged from
# plasma-wayland-protocols v1.23.0 (commit c5ac4db818f4a575a6ccfe0065b73ecfaba6e93e);
# the protocols are private to Plasma, so keep this copy pinned and re-test
# with kwin-output-tool after updating it.
set(KWIN_PROTOCOL_SOURCES)
foreach(protocol kde-output-device-v2 kde-output-management-v2)
  set(xml ${CMAKE_CURRENT_SOURCE_DIR}/protocols/${protocol}.xml)
  set(header ${CMAKE_CURRENT_BINARY_DIR}/${protocol}-client-protocol.h)
  set(code ${CMAKE_CURRENT_BINARY_DIR}/${protocol}-protocol.c)
  add_custom_command(
    OUTPUT ${header}
    COMMAND ${WAYLAND_SCANNER_EXECUTABLE} client-header ${xml} ${header}
    DEPENDS ${xml}
    VERBATIM
  )
  add_custom_command(
    OUTPUT ${code}
    COMMAND ${WAYLAND_SCANNER_EXECUTABLE} private-code ${xml} ${code}
    DEPENDS ${xml}
    VERBATIM
  )
  list(APPEND KWIN_PROTOCOL_SOURCES ${header} ${code})
endforeach()

# Plain C++ client without Qt, shared by the application and the test tool.
add_library(kwinoutputclient STATIC KWinOutputClient.cpp KWinOutputClient.h ${KWIN_PROTOCOL_SOURCES})
set_target_properties(kwinoutputclient PROPERTIES AUTOMOC OFF)
target_include_directories(kwinoutputclient PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(kwinoutputclient PUBLIC PkgConfig::WAYLAND_CLIENT)

# Command line tool to check the protocol against the running compositor
# (not built by default): list outputs and modes, switch and restore a mode.
add_executable(kwin-output-tool EXCLUDE_FROM_ALL KWinOutputTool.cpp)
set_target_properties(kwin-output-tool PROPERTIES AUTOMOC OFF)
target_link_libraries(kwin-output-tool kwinoutputclient)

add_sources(DisplayManagerKWin.cpp DisplayManagerKWin.h)
