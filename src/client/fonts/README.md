# Fonts

The fonts `am-ui` draws with. They're compiled into the program
(`am_embed_file()` in `cmake/EmbedFile.cmake`), so nothing has to be installed
next to it.

| File | Font | Used for | License |
| --- | --- | --- | --- |
| `AtkinsonHyperlegible-*.ttf` | [Atkinson Hyperlegible](https://www.brailleinstitute.org/freefont/), Braille Institute | body text, labels | SIL OFL 1.1, `AtkinsonHyperlegible-OFL.txt` |
| `InstrumentSerif-*.ttf` | [Instrument Serif](https://github.com/Instrument/instrument-serif) | big titles | SIL OFL 1.1, `InstrumentSerif-OFL.txt` |
| `NotoSans-Regular.ttf`, `NotoSerif-Regular.ttf` | [Noto](https://github.com/notofonts/latin-greek-cyrillic), the Noto Project | letters the two above lack: Greek, Cyrillic, Vietnamese, more Latin | SIL OFL 1.1, `Noto-OFL.txt` |

All of them come from [google/fonts](https://github.com/google/fonts) at commit
`e7db6ca603fd4862010383ff17ffd6cc4ffac1c0` (`ofl/atkinsonhyperlegible/`,
`ofl/instrumentserif/`, `ofl/notosans/` and `ofl/notoserif/`). Atkinson
Hyperlegible and Instrument Serif are unchanged.

Google ships Noto only as variable fonts (one file holding every weight and
width, about 2 MB each). We keep just the regular style, which is about a
third of that, made with [fontTools](https://github.com/fonttools/fonttools):

```sh
fonttools varLib.instancer 'NotoSans[wdth,wght].ttf' wght=400 wdth=100 --static --update-name-table -o NotoSans-Regular.ttf
fonttools varLib.instancer 'NotoSerif[wdth,wght].ttf' wght=400 wdth=100 --static --update-name-table -o NotoSerif-Regular.ttf
```

`theme.cpp` merges them in behind each of our fonts, so a title in Greek or
Russian shows up instead of as boxes. Chinese, Japanese and Korean aren't
covered yet: a font with those is 15 MB or more, too big to build in.

The OFL allows bundling a font with a program, as long as its license comes
along and the font isn't sold on its own. A release has to ship the three
`-OFL.txt` files (see "Licenses for what we bundle" in `docs/release-checklist.md`).
