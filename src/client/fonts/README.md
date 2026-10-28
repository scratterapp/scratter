# Fonts

The fonts `am-ui` draws with. They're compiled into the program
(`am_embed_file()` in `cmake/EmbedFile.cmake`), so nothing has to be installed
next to it.

| File | Font | Used for | License |
| --- | --- | --- | --- |
| `AtkinsonHyperlegible-*.ttf` | [Atkinson Hyperlegible](https://www.brailleinstitute.org/freefont/), Braille Institute | body text, labels | SIL OFL 1.1, `AtkinsonHyperlegible-OFL.txt` |
| `InstrumentSerif-*.ttf` | [Instrument Serif](https://github.com/Instrument/instrument-serif) | big titles | SIL OFL 1.1, `InstrumentSerif-OFL.txt` |

Both were taken unchanged from [google/fonts](https://github.com/google/fonts)
at commit `e7db6ca603fd4862010383ff17ffd6cc4ffac1c0` (`ofl/atkinsonhyperlegible/`
and `ofl/instrumentserif/`).

The OFL allows bundling a font with a program, as long as its license comes
along and the font isn't sold on its own. A release has to ship the two
`-OFL.txt` files (see "Licenses for what we bundle" in `docs/release-checklist.md`).
