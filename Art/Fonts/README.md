# Art/Fonts — the menu typeface, in source form

`Scripts/generate-menu-widgets.py` imports whichever file is named by
**`TraceMenuArtStyle::MenuFontSourceFile`** (in
`Source/Trace/UI/Widgets/Menu/TraceMenuArtStyle.h`) from this directory to the
UFont asset `/Game/Trace/UI/Fonts/F_TraceMenu`, and bakes that asset into every
text block of `WBP_TitleMenu`. That header constant is the only place this
project names a font; swapping the typeface is that one line plus a re-run.

## What is here, and under what licence

| File | Face | Licence |
|---|---|---|
| `Lato-Regular.ttf` | Lato Regular, version 2.007 (2014-02-27) | SIL Open Font License 1.1 |

Copyright and licence, read verbatim out of the file's own name table
(nameID 0 / 13 / 14):

> Copyright (c) 2011-2014 by tyPoland Lukasz Dziedzic
> (http://www.typoland.com/) with Reserved Font Name "Lato". Licensed under the
> SIL Open Font License, Version 1.1 (http://scripts.sil.org/OFL).

The OFL permits bundling and embedding, which is what the imported
`F_TraceMenu_Face` asset does — it carries the font bytes inside the .uasset —
and it requires this notice to travel with them. That is why this file exists.

## Lato is a STAND-IN and is measurably not the artist's face

The user supplied twelve candidates. Each baked word on the artist's sheet
(SETTINGS, PLAY, KEYBIND, KEY) was segmented into letters and every candidate
glyph scored by IoU at matched cap height: Lato-Regular 0.5645, Myriad Pro
Regular 0.5595, everything else below 0.48. The sheet's face is a
hairline-weight, wide, squared-off geometric techno design; Lato is a humanist
text face, heavier and rounder. It is the closest candidate that is also safe to
ship.

**Never add MyriadPro, ProximaNova or TrajanPro to this directory or to the
repository — they are commercial.** The same applies to any font whose licence
forbids embedding: the imported .uasset embeds the file's bytes, so importing it
here is redistribution.

## Known repository issue (not fixed here — flagged to the orchestrator)

`.gitignore:204-206` ignores `Art/Fonts/*.otf`, `*.ttf` and `*.pdf` wholesale, so
`Lato-Regular.ttf` is **not committed** even though its licence allows it. The
game still works from a clean clone, because the imported
`Content/Trace/UI/Fonts/F_TraceMenu_Face.uasset` carries the font data and is not
ignored — but nobody can re-run the import without fetching Lato again. The
one-line fix belongs to whoever owns `.gitignore`:

    !Art/Fonts/Lato-Regular.ttf
