# Fonts in Trace

## What is actually on screen today

**The menu typeface is Lato Regular**, imported as an ordinary runtime `UFont` at
`/Game/Trace/UI/Fonts/F_TraceMenu`. It is the *interim* face chosen by spec v20
§1: of the twelve candidates the owner supplied it measured closest to the
artist's lettering, and it is SIL Open Font Licence, so the `.ttf` itself is
committed at `Art/Fonts/Lato-Regular.ttf` (see the negation rule in
`.gitignore`). Swapping it is one line — `MenuFontSourceFile` in
`Source/Trace/UI/Widgets/Menu/TraceMenuArtStyle.h`.

The rest of this document describes **Sofachrome**, the artist's real face, and a
bitmap-atlas route for shipping it without embedding it. That route is **designed
but not wired up**, and the section below records the engine fact that has to be
solved before it can be:

> **An "offline" (bitmap-atlas) `UFont` cannot drive the UMG title screen.**
> `UFont::GetCompositeFont` returns `nullptr` unless `FontCacheType == Runtime`
> (`Engine/Private/Font.cpp`), and `FSlateFontInfo` then silently substitutes
> Slate's last-resort face. So an atlas font can work for the Canvas screens and
> will quietly do nothing for every text block in `WBP_TitleMenu`. Measured
> during the spec v20 pass, not assumed.

Status of the atlas pipeline, so nobody follows a dead path:

| Piece | State |
|---|---|
| `Scripts/generate_font_atlas.py` | exists; writes the PNG + JSON described below |
| `Content/Trace/UI/Fonts/Source/T_FontAtlas.{png,json}` | generated, committed |
| `Scripts/import-font-atlas.sh` | **does not exist** |
| Any C++ referencing the atlas | **none** — `grep -rn FontAtlas Source/` is empty |

So the atlas is currently art on disk, not a font the game uses. Reconcile it
with the runtime-`UFont` route before calling either one done.

## Why the atlas route was designed this way

Sofachrome ships under Typodermic's **free desktop licence**. The licence
enumerates what you may and may not do, and the relevant lines are:

> **Allowed:** … web page (not embedded), **app (not embedded)**, PDF, eBook
> cover or images (not embedded)
>
> **Not allowed:** **app (embedded)**, web page (embedded), eBook (embedded),
> product creation platform, … OEM device embedding

A game is an app. Shipping the `.otf` inside the build is embedding, and that is
explicitly forbidden. On top of that **this repository is public**, so committing
the font would be redistribution as well as embedding.

Rendering the typeface to an image with a licensed desktop installation, and
shipping the image, is the "app (not embedded)" case — and it is the same route
the artist's existing sprite sheet took (the baked PLAY / SETTINGS / KEYBIND /
KEY words were made that way).

## The honest caveat

**A full-alphabet atlas is a greyer position than pre-rendering a fixed list of
words.** A texture of every glyph plus metrics is enough to set arbitrary text,
which is arguably the capability the "not embedded" clause exists to withhold.
This was raised with the project owner, who chose the atlas deliberately for the
consistency it buys.

If Trace is ever distributed commercially, or the licensing position needs to be
defensible rather than merely arguable:

1. Buy Typodermic's **app licence** for Sofachrome — they sell one, and the
   `read-this.html` in the font download links to it.
2. Then embed the real font: import the `.otf` as a normal runtime `UFont` and
   point the single font constant in `TraceMenuArtStyle` at it.

That is a small change on purpose. The constant exists so the licensing decision
is one line of code, not a rewrite.

## What the atlas is

`Scripts/generate_font_atlas.py` writes two files into
`Content/Trace/UI/Fonts/Source/`:

| File | What it is |
|---|---|
| `T_FontAtlas.png` | 2048x1024 RGBA, 95 glyphs (printable ASCII), white on transparent |
| `T_FontAtlas.json` | per-glyph `u, v, uSize, vSize` plus `ascent`, `descent`, `lineHeight` |

Every cell is a **full advance width by a full line height**, so the engine can
advance the pen by the cell width and sit every glyph on a shared baseline with
no per-glyph bearing table. That is deliberate: it is exactly what Unreal's
offline font format can express.

Regenerate at a different size with `--size` (default 96 px em — menu text is
20-32 px at 1080p, so there is room to downscale cleanly). `--preview` writes a
sheet of real strings composed **from the metrics**, so what you look at is what
the engine will draw.

## Fonts that were evaluated and rejected

The owner supplied twelve candidates in `font files test.zip`. None matched the
artist's baked lettering; the match was measured, not eyeballed — each baked word
was segmented into letters and every candidate glyph compared by IoU at matched
cap height. Best was Lato Regular at 0.56, and it is stylistically generic next
to the artist's squared techno face — which is why it is described everywhere as
the *interim* face and not as the answer.

**Three of those twelve are commercial and must not be committed here:**
Myriad Pro, Proxima Nova and Trajan Pro 3. Lato, Bebas Neue, Raleway, Roboto and
Roboto Slab are OFL or Apache and would be safe to commit if ever needed —
`Lato-Regular` is the fallback for exactly that reason.
