# Embedded reader UI font

`reader_ui_24.c` is generated from the repository's OFL-licensed Noto Sans SC Medium.
License: `assets/source/fonts/Noto-CJK-OFL.txt`.
Rebuild with `node scripts/generate_reader_font.cjs` after installing the pinned
`tools/ui-assets` dependencies. The glyph set is extracted from the reader's UI/error text.

This small embedded UI font keeps transfer, errors, controls and wake-up readable while SD
font files are closed for USB/browser import. Book titles and body text still use the user's
selected SD font via the bounded streaming FreeType cache.
