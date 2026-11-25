# am-ui, the app window. SDL3 opens the window, reads input and draws (through
# SDL_Renderer, which picks OpenGL or Vulkan on Linux and Direct3D on Windows by
# itself), and Dear ImGui lays out the widgets. Both come in with FetchContent,
# pinned by commit like Catch2 in tests/CMakeLists.txt, so CI, Arch and Windows
# build the same code.
include(FetchContent)

# Only the parts of SDL a window needs. Audio stays off on purpose: the daemon
# owns the sound output, and SDL's audio would open PipeWire for nothing.
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
set(SDL_AUDIO OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
set(SDL_HAPTIC OFF CACHE BOOL "" FORCE)
set(SDL_SENSOR OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    # release-3.4.18, by commit.
    GIT_TAG 829a65d769d935c4852f8159e964312c0957260a
    SYSTEM
)
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    # v1.92.9b, by commit.
    GIT_TAG f1cc2ae15e53a861a874c3034aae6798fde194ab
    SYSTEM
)
# ImGui has no CMakeLists.txt of its own, so this only downloads it.
FetchContent_MakeAvailable(SDL3 imgui)

# ImGui and its two SDL3 backends. Built without am_enable_warnings(): ImGui's
# code doesn't pass -Wconversion -Werror, and it isn't ours to change. It does
# build without exceptions and RTTI, like everything else.
add_library(am_imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp
)
target_include_directories(am_imgui SYSTEM PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_compile_definitions(am_imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
target_link_libraries(am_imgui PUBLIC SDL3::SDL3-static)
if(MSVC)
    target_compile_options(am_imgui PRIVATE /EHs-c- /GR-)
    target_compile_definitions(am_imgui PRIVATE _HAS_EXCEPTIONS=0)
else()
    target_compile_options(am_imgui PRIVATE -fno-exceptions -fno-rtti)
endif()

# FreeType draws the letters when it's there. ImGui's own rasterizer
# (stb_truetype) doesn't hint, so the edges of small letters fall between
# pixel rows and body text looks soft, grey text on the side bar most of all.
# FreeType's light hinting lines each letter up with the pixel rows and keeps
# its shape across. It comes from the system through pkg-config (freetype2 on
# Arch, libfreetype-dev on Ubuntu, and nearly every Linux desktop has it).
# Without it, as on Windows for now, ImGui falls back to stb_truetype.
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(FREETYPE QUIET IMPORTED_TARGET freetype2)
endif()
# The header is only declarations, so theme.cpp can name the hinting flag
# whichever rasterizer is built in.
target_include_directories(am_imgui SYSTEM PUBLIC ${imgui_SOURCE_DIR}/misc/freetype)
if(FREETYPE_FOUND)
    target_sources(am_imgui PRIVATE ${imgui_SOURCE_DIR}/misc/freetype/imgui_freetype.cpp)
    target_compile_definitions(am_imgui PUBLIC IMGUI_ENABLE_FREETYPE)
    target_link_libraries(am_imgui PUBLIC PkgConfig::FREETYPE)
    message(STATUS "am-ui draws text with FreeType")
else()
    message(STATUS "am-ui draws text with stb_truetype (FreeType not found)")
endif()

add_executable(am-ui
    main_client.cpp
    theme.cpp
    views/drawing.cpp
    views/now_playing.cpp
    views/spectrum.cpp
    views/waiting_view.cpp
)
target_link_libraries(am-ui PRIVATE am_client_core am_imgui am_net am_pal am_common)
am_enable_warnings(am-ui)
# Built next to the daemon (the top of the build folder), not in src/client/:
# the daemon starts the am-ui that sits beside it when the desktop asks to see
# the player and no window is open (issue #41), as it does once installed.
set_target_properties(am-ui PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")

# On Windows: the icon, and in Release a GUI program (WIN32_EXECUTABLE), so
# starting it from Explorer or the Start menu doesn't open a console window as
# well. SDL_main.h, included in main_client.cpp, supplies the WinMain that needs.
# Debug keeps the console, where the "am-ui: ..." error messages go.
if(WIN32)
    target_sources(am-ui PRIVATE "${PROJECT_SOURCE_DIR}/packaging/windows/am-ui.rc")
    set_target_properties(am-ui PROPERTIES WIN32_EXECUTABLE $<NOT:$<CONFIG:Debug>>)
endif()

# The fonts, compiled in (fonts/README.md says where they come from).
include(EmbedFile)
am_embed_file(am-ui ${CMAKE_CURRENT_SOURCE_DIR}/fonts/AtkinsonHyperlegible-Regular.ttf atkinson_regular)
am_embed_file(am-ui ${CMAKE_CURRENT_SOURCE_DIR}/fonts/AtkinsonHyperlegible-Bold.ttf atkinson_bold)
am_embed_file(am-ui ${CMAKE_CURRENT_SOURCE_DIR}/fonts/InstrumentSerif-Regular.ttf instrument_serif_regular)
am_embed_file(am-ui ${CMAKE_CURRENT_SOURCE_DIR}/fonts/InstrumentSerif-Italic.ttf instrument_serif_italic)
am_embed_file(am-ui ${CMAKE_CURRENT_SOURCE_DIR}/fonts/NotoSans-Regular.ttf noto_sans)
am_embed_file(am-ui ${CMAKE_CURRENT_SOURCE_DIR}/fonts/NotoSerif-Regular.ttf noto_serif)
