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

add_executable(am-ui
    main_client.cpp
    theme.cpp
    views/waiting_view.cpp
)
target_link_libraries(am-ui PRIVATE am_client_core am_imgui am_net am_pal am_common)
am_enable_warnings(am-ui)

# The fonts, compiled in (fonts/README.md says where they come from).
include(EmbedFile)
am_embed_file(am-ui ${CMAKE_CURRENT_SOURCE_DIR}/fonts/AtkinsonHyperlegible-Regular.ttf atkinson_regular)
am_embed_file(am-ui ${CMAKE_CURRENT_SOURCE_DIR}/fonts/AtkinsonHyperlegible-Bold.ttf atkinson_bold)
am_embed_file(am-ui ${CMAKE_CURRENT_SOURCE_DIR}/fonts/InstrumentSerif-Regular.ttf instrument_serif_regular)
am_embed_file(am-ui ${CMAKE_CURRENT_SOURCE_DIR}/fonts/InstrumentSerif-Italic.ttf instrument_serif_italic)
