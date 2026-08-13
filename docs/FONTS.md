# Fonts in Trace

## What is on screen

**The menu and HUD typeface is Sofachrome**, in two real weights — no synthesis:

| Atlas | Font file | Used for |
|---|---|---|
| `T_FontAtlas` | `Sofachrome W05 ExtraLight.ttf` (weight 250) | everything by default |
| `T_FontAtlasBold` | `Sofachrome Rg.otf` (weight 400) | character names on the select screen |

Measured against the artist's own baked lettering, whose strokes are **10.3% of
cap height**: ExtraLight is **16.9%**, Regular is **66.2%**. ExtraLight is the
face the menu art was designed in.

Text is drawn from a **bitmap glyph atlas** — a texture of every letter plus a
metrics file — by two renderers in `Source/Trace/UI/Text/`: a Canvas blitter and
a UMG widget. Neither constructs an `FSlateFontInfo`, deliberately (see below).

**The font files are NOT in this repository and must never be committed.** They
are gitignored. To work on the UI, put your own licensed copies in `Art/Fonts/`
and run:

    python3 Scripts/generate_font_atlas.py --font "Art/Fonts/Sofachrome W05 ExtraLight.ttf" --name T_FontAtlas --preview
    python3 Scripts/generate_font_atlas.py --font "Art/Fonts/Sofachrome Rg.otf"            --name T_FontAtlasBold
    Scripts/import-font-atlas.sh

Skip it and the menu falls back to Lato. Nothing breaks; it just looks wrong.

## Do not try to synthesise a weight

`generate_font_atlas.py` has a `--thin` option that erodes glyphs to fake a
lighter cut. **It defaults to 0 and should stay there.** Two attempts to use it
damaged the letterforms: the first clipped glyphs outright (PLAY lost the A's
left diagonal and the Y became a stub), and even once that was fixed, uniform
erosion past one pixel DELETED `( ) [ ] { } /` and `\` entirely — and the HUD
draws `[E]`. Use a real weight instead. That is what ExtraLight is for.

## An engine fact that shaped the design

An "offline" (bitmap) `UFont` **cannot drive UMG**. `UFont::GetCompositeFont`
returns `nullptr` unless `FontCacheType == Runtime` (`Engine/Private/Font.cpp`),
and `FSlateFontInfo` then silently substitutes Slate's last-resort face — so an
atlas font would look correct on the Canvas screens and be quietly wrong on the
UMG title screen. That is why the glyphs are drawn directly instead.

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

Cells are packed with a **16 px transparent gutter** (`PAD` in
`generate_font_atlas.py`) and imported **with a mip chain**
(`TMGS_SIMPLE_AVERAGE`). Both halves of that are load-bearing and they must move
together. The HUD draws this 96 px em at ~10 px; with no mips each screen pixel
took roughly one arbitrary texel, and in an extra-light face the horizontal bars
are one texel tall — so the title screen drew "MOVE" as "MOVC" and "ESC" as
"CSC". Mips fix it, but a mip averages **across** the gutter, so the old 2 px
one would have pulled the next letter into this one. 16 px covers mip 3, i.e.
down to a 12 px em.

**The two weights do not share advances.** They are two different font files, so
94 of the 95 cells differ and bold is roughly half again as wide. Layout and
`TraceText::MeasureWidth()` are both face-relative and answer correctly — but
only if you hand them the weight you are going to **draw** in. Measuring light
and drawing bold is how the character name overran its column on the select
screen.

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
