# Fonts on the TV

Inventory taken directly from the configured TV on 2026-09-19. Fonts live in
`/usr/share/fonts`, and the UI renderer rasterises them with the TV's own
`libfreetype.so.6`.

The native UI face is available in a useful complete family:

```
LG_Smart_UI-Light.ttf
LG_Smart_UI-Regular.ttf
LG_Smart_UI-SemiBold.ttf
LG_Smart_UI-Bold.ttf
LG_Smart_UI-{Light,Regular,SemiBold,Bold}Oblique.ttf
LG_Smart_UI_Condensed-{Light,Regular,SemiBold,Bold}.ttf
```

There are also LG Smart UI faces for Arabic/Hebrew/Thai, Amharic, Bengali,
Devanagari, Gujarati, Gurmukhi, Japanese, Kannada, Malayalam, Oriya, Simplified
and Traditional Chinese, Tamil, Telugu and Urdu. Other installed families
include Droid Sans/Serif/Mono/Kufi/Naskh and Droid Sans Fallback, Nanum Gothic,
Noto Emoji/Color Emoji, Museo Sans, Miso, Tinos, the LG Display families and
Sandstone Icons.

The UI selects fonts in this order:

1. `UI_FONT=/absolute/path.ttf`, when set;
2. `/usr/share/fonts/LG_Smart_UI-Regular.ttf` on the TV;
3. Droid Sans;
4. common DejaVu/Liberation Sans paths for host development.

The selected path is printed at startup. Glyphs are multi-channel distance
fields (MTSDF, `src/ui/msdf.c`) generated from FreeType's outlines at 48 px;
the atlas prewarms printable ASCII, then adds glyphs on demand.

A character none of the loaded faces has is looked up in the font index
(`src/ui/font_index.c`): every font under `/usr/share/fonts` is read once, on a
thread started at launch, as ranges of the characters it maps, and the first
font with the character joins the atlas. That is how Thai, Arabic, the Indic
scripts and CJK find the TV's own faces for them. Regular cuts are tried before
bold, italic and condensed ones, sans before serif.

UI labels are drawn a glyph per codepoint. Subtitles drawn as plain text are
shaped with HarfBuzz, so stacked Thai marks, joined Arabic and Indic conjuncts
come out right; a line mixing directions is not reordered (no bidi).

libass has no font provider here, so every style resolves to the UI face.
When a script needs characters the UI face lacks, `subs.c` registers the font
the index names as a memory font and makes it libass's default *family*: libass
tries it for each missing glyph and still falls back to the UI face for the
rest.

To repeat the inventory on another set:

```sh
find /usr/share/fonts -type f \( -iname '*.ttf' -o -iname '*.otf' -o -iname '*.ttc' \) | sort
```
