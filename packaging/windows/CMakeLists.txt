# What `cmake --install` puts in place on Windows so Scratter starts like a
# normal app (issue #97; Linux's version is packaging/linux, from #41):
#
#   <prefix>\scratterd.exe, <prefix>\am-ui.exe    the two programs, side by side
#   <Start Menu>\Programs\Scratter.lnk            the shortcut, with am-ui.exe's icon
#
# The programs share one folder because the daemon starts the window from next
# to itself (AM_UI_NAME, launch_ui()). SDL3 is linked in statically, so neither
# needs a DLL of ours; they use the Visual C++ runtime that Visual Studio and
# most other programs install.
#
# The default prefix is %LOCALAPPDATA%\Programs\Scratter, the per-user programs
# folder Windows itself suggests (FOLDERID_UserProgramFiles), so installing
# needs no admin rights. `cmake --install <build> --prefix <folder>` puts it
# anywhere else.
#
# The window needs no AppUserModelID to share the shortcut's icon and name on
# the taskbar: SDL doesn't set one, and without one Windows matches a window
# with a shortcut by the program's path.
#
# Nothing here uninstalls. Delete the folder and the shortcut by hand.

# "scratter" -> "Scratter", the name people see in the Start Menu (the same as
# packaging/linux).
string(SUBSTRING "${AM_APP_NAME}" 0 1 first)
string(SUBSTRING "${AM_APP_NAME}" 1 -1 rest)
string(TOUPPER "${first}" first)
set(AM_DISPLAY_NAME "${first}${rest}")

if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT AND DEFINED ENV{LOCALAPPDATA})
    file(TO_CMAKE_PATH "$ENV{LOCALAPPDATA}/Programs/${AM_DISPLAY_NAME}" default_prefix)
    set(CMAKE_INSTALL_PREFIX "${default_prefix}" CACHE PATH "Where cmake --install puts Scratter"
        FORCE)
endif()

if(DEFINED ENV{APPDATA})
    file(TO_CMAKE_PATH "$ENV{APPDATA}/Microsoft/Windows/Start Menu/Programs" default_start_menu)
else()
    set(default_start_menu "")
endif()
set(AM_START_MENU_DIR "${default_start_menu}" CACHE PATH
    "Folder cmake --install puts the Scratter shortcut in; empty for no shortcut")

install(TARGETS ${AM_DAEMON_TARGET} am-ui RUNTIME DESTINATION .)

if(AM_START_MENU_DIR)
    # This code runs at install time. ${...} is filled in now, \${...} at
    # install time, so the shortcut points wherever --prefix put am-ui.
    install(CODE "
        file(TO_NATIVE_PATH \"\${CMAKE_INSTALL_PREFIX}/$<TARGET_FILE_NAME:am-ui>\" target)
        file(TO_NATIVE_PATH \"${AM_START_MENU_DIR}/${AM_DISPLAY_NAME}.lnk\" shortcut)
        message(STATUS \"Shortcut: \${shortcut}\")
        execute_process(
            COMMAND powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass
                    -File \"${CMAKE_CURRENT_SOURCE_DIR}/make-shortcut.ps1\"
                    -Shortcut \"\${shortcut}\" -Target \"\${target}\"
                    -Description \"${AM_DISPLAY_NAME}, an Apple Music player\"
            RESULT_VARIABLE failed)
        if(failed)
            message(FATAL_ERROR \"Couldn't create the Start Menu shortcut \${shortcut}\")
        endif()
    ")
endif()
