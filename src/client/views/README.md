# views

Dear ImGui views for `am-ui` (Library, Search, Queue, Spectrum). See `docs/SPECIFICATION.md`
section 3.4.2.

- `waiting_view.cpp`: what the window shows while the daemon isn't connected.
- `now_playing.cpp`: what's playing, the position, play/pause, stop and volume.
- `drawing.cpp`: the background record and the app label, shared by both.

Each view draws with the colors and fonts in `../theme.hpp` instead of its own.
Owners: @cocoa55 @kerelosgayed
