# What `cmake --install` puts in place on Linux so Scratter behaves like a
# normal desktop app (issue #41):
#
#   bin/scratterd, bin/am-ui                       the two programs, side by side
#   share/applications/scratter.desktop            the app launcher entry
#   share/icons/hicolor/<size>/apps/scratter.png   its icon, from icons/
#
# The .desktop file name is the app id am-ui sets (SDL_HINT_APP_ID, from
# AM_APP_NAME). That's how GNOME and KDE match the open window with its
# launcher entry and show its icon in the taskbar instead of a generic one.
#
# The .desktop file is filled in at install time, not configure time, so the
# path to am-ui in it is right for whatever `cmake --install --prefix` says.

include(GNUInstallDirs)

install(TARGETS ${AM_DAEMON_TARGET} am-ui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# "scratter" -> "Scratter", the name people see in the launcher.
string(SUBSTRING "${AM_APP_NAME}" 0 1 first)
string(SUBSTRING "${AM_APP_NAME}" 1 -1 rest)
string(TOUPPER "${first}" first)
set(AM_DISPLAY_NAME "${first}${rest}")

foreach(size 16 22 24 32 48 64 128 256)
    install(FILES icons/${size}.png
            DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${size}x${size}/apps
            RENAME ${AM_APP_NAME}.png)
endforeach()

set(desktop_out "${CMAKE_CURRENT_BINARY_DIR}/${AM_APP_NAME}.desktop")
# This code runs at install time. ${...} is filled in now (the source and
# build folders), \${...} at install time (the prefix). configure_file() then
# replaces the @NAME@ markers in the template with the variables set here.
install(CODE "
    set(bin \"\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}\")
    set(AM_APP_NAME \"${AM_APP_NAME}\")
    set(AM_DISPLAY_NAME \"${AM_DISPLAY_NAME}\")
    set(AM_UI_PATH \"\${bin}/$<TARGET_FILE_NAME:am-ui>\")
    configure_file(\"${CMAKE_CURRENT_SOURCE_DIR}/app.desktop.in\" \"${desktop_out}\" @ONLY)
    file(INSTALL \"${desktop_out}\"
         DESTINATION \"\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DATADIR}/applications\")
")
