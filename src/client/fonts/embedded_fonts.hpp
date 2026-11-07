#pragma once

// The fonts am-ui carries inside itself, so it never has to find them on disk.
// src/client/CMakeLists.txt turns each .ttf in this folder into one of these
// functions with am_embed_file() (cmake/EmbedFile.cmake). All three families are
// under the SIL Open Font License, which lets us ship them with the program;
// the license texts sit next to the fonts.

#include <span>

namespace am::embedded {

// Atkinson Hyperlegible, by the Braille Institute: made to stay readable for
// people with low vision. Everything that isn't a headline.
std::span<const unsigned char> atkinson_regular();
std::span<const unsigned char> atkinson_bold();

// Instrument Serif: a narrow display serif, for the big titles.
std::span<const unsigned char> instrument_serif_regular();
std::span<const unsigned char> instrument_serif_italic();

// Noto Sans and Noto Serif, regular only: the letters the two above don't
// have (Greek, Cyrillic, Vietnamese...), merged in behind them by theme.cpp.
std::span<const unsigned char> noto_sans();
std::span<const unsigned char> noto_serif();

} // namespace am::embedded
