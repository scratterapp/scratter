# The app window, explained

This covers `am-ui`'s first real code (issue #35): opening a window with SDL3,
drawing it with Dear ImGui, a render loop that sleeps when there's nothing new
to draw, and how the window got its look. The code is in `src/client/`:

| File | What it does |
| --- | --- |
| `main_client.cpp` | Starts SDL, makes the window and renderer, sets up ImGui, runs the loop |
| `frame_pacer.hpp` | Decides when to draw a frame and how long to sleep in between |
| `theme.hpp`, `theme.cpp` | Colors, spacing and fonts |
| `views/waiting_view.cpp` | What the window shows while nothing plays |
| `fonts/` | The two font families, compiled into the program |
| `CMakeLists.txt` | Fetches SDL3 and ImGui and builds `am-ui` |

The window doesn't talk to the daemon yet; that's #36.

## Three libraries, three jobs

Putting pixels in a window takes three separate jobs, and each one is a
different library here:

1. **Getting a window from the desktop, and hearing about input.** On Linux
   that means talking to a Wayland compositor or an X11 server; on Windows,
   calling `CreateWindowEx` and running a message loop. **SDL3** does this for
   both. It also tells us when the window is resized, covered, minimized or
   closed, and when keys and the mouse do anything.
2. **Deciding what goes where.** **Dear ImGui** does this. Every frame we call
   functions like `ImGui::TextUnformatted("Nothing playing")` and
   `ImGui::Button("Play")`, and ImGui turns that into a list of triangles with
   colors and texture coordinates. It never touches the screen itself.
3. **Turning triangles into pixels.** That's the GPU's work, through a graphics
   API: OpenGL or Vulkan on Linux, Direct3D on Windows. **SDL_Renderer**, which
   is part of SDL3, picks one of them by itself and draws ImGui's triangles
   with it.

ImGui connects to SDL through two small "backend" files that ship with ImGui:
`imgui_impl_sdl3.cpp` feeds SDL's input events to ImGui, and
`imgui_impl_sdlrenderer3.cpp` hands ImGui's triangles to SDL_Renderer.

Because SDL hides both the windowing system and the graphics API, there's no
`#ifdef` and no code in `src/pal/` for the window. That answered two open
questions at once: GLFW or SDL3 (SDL3, mainly for its better Wayland support),
and where the graphics backend lives (inside SDL). The spec wanted hand-written
Vulkan and DirectX 11 code; a music player draws a few hundred triangles, so
that would be two drawing layers to maintain for nothing. If a measurement ever
says otherwise, ImGui has Vulkan and DirectX backends too, and swapping one in
only touches `main_client.cpp`.

## Immediate mode

Most UI toolkits (Qt, GTK, the browser's DOM) are **retained mode**: you build
a tree of widget objects once, keep it around, and change it when something
happens. ImGui is **immediate mode**: there are no widget objects. Each frame,
your code describes the whole screen from scratch:

```cpp
ImGui::Begin("waiting", nullptr, kFlags);
ImGui::TextUnformatted("Nothing playing");
if (ImGui::Button("Play")) {   // true on the frame it was clicked
    send_play();
}
ImGui::End();
```

The screen always shows exactly what the code says, from whatever state the
program has right now, so there's no second copy of the state in widgets to
keep in sync. The cost is that drawing a frame means running all of that code
again, which is why the next part matters.

## A loop that sleeps

A game redraws 60 or more times a second, whether or not anything changed.
For a music player, almost every one of those frames would be identical to the
last, and each one wakes the CPU and the GPU. So `am-ui` only draws when the
picture might change. `FramePacer` holds the rules:

| Situation | What the loop does |
| --- | --- |
| Window minimized, hidden or fully covered | Draw nothing. Wake at most every 250 ms |
| Something animates (the spectrum, later) | Draw every frame. vsync caps it at the screen's refresh rate |
| Playing, nothing animating (#37) | Draw after each event, plus once on every whole second for the clock |
| Nothing playing, as today | Draw after an event, then sleep until the next one |

An "event" is anything SDL reports: the mouse moved, a key, a resize, the
window being uncovered. Each one buys two frames, not one, because ImGui
sometimes needs a frame to notice a change (say, that the mouse is now over a
button) and a second to draw it.

The sleeping happens in `SDL_WaitEventTimeout()`. Under it, SDL asks the
kernel (with `poll()` on Linux) to wake the process when the Wayland or X11
connection has something to read, or when the timeout runs out. In OSTEP's
terms (chapter 4, process states), the process is now **blocked**: the
scheduler won't give it the CPU until one of those things happens. That's why a
waiting window costs 0% CPU, not "a little". Compare a loop that calls
`SDL_PollEvent()` and finds nothing: the process stays **ready** and keeps
running, using a whole core to ask "anything yet?" millions of times.

`FramePacer` knows nothing about SDL. `main_client.cpp` tells it the time and
what happened, and asks it "should I draw?" and "how long may I sleep?". That
split is what lets `tests/test_frame_pacer.cpp` check every rule without
opening a window, the same trick `amctl_args.hpp` uses for parsing.

The 1 Hz tick only runs while something plays. An earlier version ticked all
the time, and on a machine without a GPU (Mesa's software renderer, which the
cloud container uses) redrawing the record once a second cost about 5% of a
core. Nothing changes on screen when nothing plays, so now the window doesn't
redraw at all then.

## Cleaning up in the right order

SDL, the renderer, the window and ImGui each have to be shut down, in the
reverse order they were made: ImGui's backends draw with the renderer, so they
must go before it; the renderer draws into the window, so it goes before the
window; and `SDL_Quit()` goes last. `main_client.cpp` gets this right on every
early `return` without writing the cleanup out each time, using RAII (as in
[shm-mapping.md](shm-mapping.md)):

- `WindowPtr` and `RendererPtr` are `std::unique_ptr`s with SDL's destroy
  function as their deleter.
- `SdlSession` and `ImGuiSession` are tiny structs whose destructors call
  `SDL_Quit()` and the ImGui shutdowns.

C++ destroys local variables in the reverse order they were declared, so
declaring them in the order they're made gives the right shutdown order for
free.

## High-DPI screens

On a high-DPI screen, one "point" of window size is 1.5 or 2 real pixels.
`SDL_WINDOW_HIGH_PIXEL_DENSITY` asks for a window that uses all of the real
pixels, so text is sharp. `SDL_GetDisplayContentScale()` says how much to
enlarge things so they're the same physical size as on a normal screen, and
`apply_style()` multiplies ImGui's spacing and font sizes by it. The views
measure their own layout in units of the font size (`em`, as in CSS), so they
grow along with it.

## How it looks, and why

The look is meant to suit a music player rather than a debugging tool, which
is what ImGui's default theme looks like:

- **Colors.** A warm near-black background (`#14110F`), paper-colored text
  (`#EFE7DC`), and one strong color, vermilion (`#FF5B2E`), like a record
  label. The accent is kept for what matters on screen (the label, later the
  play button and the progress bar), so it stays meaningful. All the colors
  are in `palette` in `theme.hpp`, and views use those names instead of
  writing their own numbers.
- **Type.** Two font families. Atkinson Hyperlegible, from the Braille
  Institute, for everything you read: it was drawn so that letters people mix
  up (I, l and 1; O and 0) look different, which helps with low vision. Instrument
  Serif, a narrow serif, for big titles only. Both use the SIL Open Font
  License, which allows shipping them inside the program
  (`src/client/fonts/README.md`).
- **Space.** Generous padding, rounded corners, no outlines.
- **The record.** Before anything plays, a record sits half off the right
  edge, drawn from about 50 circles on ImGui's draw list
  (`views/waiting_view.cpp`). Light catches its grooves in two places. It
  costs nothing while idle, because idle frames aren't drawn.

## Fonts inside the program

The fonts are compiled into `am-ui`, so it never has to find them on disk
(where would they be after installing?). `cmake/EmbedFile.cmake` reads each
`.ttf` file at build time and writes a `.cpp` file with its bytes as an array:

```cpp
std::span<const unsigned char> atkinson_regular() {
    static constexpr unsigned char kBytes[] = {0x00, 0x01, 0x00, 0x00, /* ... */};
    return kBytes;
}
```

The array ends up in the program's read-only data, like any other constant.
`load_fonts()` hands the bytes to ImGui with `FontDataOwnedByAtlas = false`,
because by default ImGui frees font data it's given, and freeing memory that
was never allocated would crash. C++26 adds `#embed`, which does this without
CMake's help; our compilers don't offer it in C++23 yet.

## Building SDL3 and ImGui

Both come in with CMake's `FetchContent`, pinned to a commit like Catch2
(`src/client/CMakeLists.txt`). Ubuntu, which CI runs, doesn't package SDL3, and
Windows has no system copy, so building it from source keeps all three machines
the same. Only the parts of SDL a window needs are built: audio is off, since the
daemon owns the sound.

ImGui is built as its own library, `am_imgui`, without our warning flags,
because its code doesn't pass `-Wconversion -Werror` and isn't ours to change.
It still builds without exceptions or RTTI, like the rest of the project.

SDL3 is written in C, so the build now uses a C compiler too. Under the
sanitizer presets, C and C++ must come from the same compiler (ASan's runtime
differs between GCC and Clang), so set both whenever you pick one:
`-DCMAKE_C_COMPILER=clang-20 -DCMAKE_CXX_COMPILER=clang++-20`.

## Testing it

- `tests/test_frame_pacer.cpp` checks each rule of the render loop.
- `am_ui_opens_and_closes` (in `tests/CMakeLists.txt`) runs
  `am-ui --frames 5` with SDL's offscreen video driver and software renderer,
  which need no screen or GPU, so it runs in CI. Under `linux-asan` it also
  catches leaks and memory errors in start-up and shutdown.
- By hand: run `build/linux-debug/src/client/am-ui`, then watch its CPU in a
  second terminal window with `top -p` followed by its process id
  (`pgrep -x am-ui` prints it). Leave the mouse off the window: it should sit at
  0.0%. Move the mouse over it and it climbs for as long as you move.

## Read more

- Spec section 3.4.2 (the render loop) and section 4 (why the window isn't in
  the PAL).
- The plan for the rest of the window: issues #36 to #44 on GitHub (phase 5
  on the roadmap issue).
- Dear ImGui's FAQ, especially "How can I have multiple fonts" and "How should
  I handle DPI": <https://github.com/ocornut/imgui/blob/master/docs/FAQ.md>.
- SDL3's wiki: <https://wiki.libsdl.org/SDL3/FrontPage>.
