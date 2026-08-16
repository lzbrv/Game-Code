#!/usr/bin/env bash
# ==============================================================================
# Trace — import-font-atlas.sh
#
# Puts the game's TYPE on screen. Runs the two halves of Scripts/import_font_atlas.py:
#
#   1. metrics  Content/Trace/UI/Fonts/Source/T_FontAtlas{,Bold,Hud}.json
#                 -> Source/Trace/UI/Text/TraceFontAtlasMetrics.h    (plain python)
#   2. texture  Content/Trace/UI/Fonts/Source/T_FontAtlas{,Bold,Hud}.png
#                 -> Content/Trace/UI/Fonts/T_FontAtlas{,Bold,Hud}.uasset  (in the editor)
#
# ------------------------------------------------------------------------------
# THREE FACES (spec v23 §A3, third face by v25 §4)
# ------------------------------------------------------------------------------
# The owner asked for the game to be set in an extra-light cut, with the character
# NAMES on the selection screen left bold, and — v25 — for the in-match HUD and the
# character ability DESCRIPTIONS to be Erbaum Bold. Each face comes from ITS OWN
# REAL FONT FILE — synthesising one by eroding another was tried twice and damaged
# the letterforms both times, so --thin is 0 for all of them and must stay there:
#
#   T_FontAtlas      Sofachrome W05 ExtraLight.ttf   the DEFAULT: menus, everything
#   T_FontAtlasBold  Sofachrome Rg.otf               character NAMES only
#   T_FontAtlasHud   Erbaum-Bold.otf                 match HUD + ability DESCRIPTIONS
#
# All three sheets go through this script together and land in ONE generated header
# as a table of faces, because the runtime picks between them per draw. They must be
# rasterised at the same --size: import_font_atlas.py refuses to emit the header if
# their em, LINE HEIGHT or charset ever disagree, since one layout pass serves all
# three. Ascent, descent, cap height and advances are per face and may differ —
# Erbaum splits the shared 116 px line box 93/23 where Sofachrome splits it 95/21.
#
# YOU DO NOT NEED TO RUN THIS TO PLAY. All outputs are committed, exactly like
# the railgun's assets. Run it after re-running Scripts/generate_font_atlas.py,
# then commit what changes under Content/Trace/UI/Fonts and Source/Trace/UI/Text.
#
# ------------------------------------------------------------------------------
# WHY THE TEXTURE AND NOT A UFont
# ------------------------------------------------------------------------------
# Because an offline (bitmap) UFont CANNOT drive UMG, and this was measured on
# this project rather than assumed: UFont::GetCompositeFont returns nullptr
# unless FontCacheType == Runtime (Engine/Private/Font.cpp), and an FSlateFontInfo
# built on one then silently draws in Slate's LAST-RESORT face. Importing the
# atlas as a font would therefore look perfect on the Canvas screens and be
# quietly wrong on the UMG title screen — the worst of the two possible bugs,
# because nothing logs and nothing crashes.
#
# So the atlas is imported as an ordinary UTexture2D and the glyphs are drawn by
# hand, one textured quad each, by Source/Trace/UI/Text. The engine never sees a
# font here at all.
#
# ------------------------------------------------------------------------------
# THE FONT FILE IS NOT IN THIS REPOSITORY, BY DESIGN
# ------------------------------------------------------------------------------
# Sofachrome and Erbaum are licensed to the owner for desktop use, not for
# embedding, and this repo is public. Only the rasterised sheets are committed.
# Regenerating one needs your own licensed copy at Art/Fonts/ (gitignored) —
# see docs/FONTS.md and the header of Scripts/generate_font_atlas.py, which also
# records that a whole-charset atlas is a GREY position that a commercial
# release should replace with a bought app licence.
# ==============================================================================
set -euo pipefail

. "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)/_trace_common.sh"

DO_EDITOR=1
DO_HEADER=1
DRY_RUN=0
ONLY_WEIGHT=""

usage() {
    cat <<EOF
${TRACE_PROJECT_NAME} import-font-atlas

Regenerates Source/Trace/UI/Text/TraceFontAtlasMetrics.h and imports
Content/Trace/UI/Fonts/T_FontAtlas, T_FontAtlasBold and T_FontAtlasHud — the
light, bold and HUD faces — from the sheets under .../Fonts/Source.

USAGE
  Scripts/import-font-atlas.sh [options]

OPTIONS
      --header-only   Only regenerate the metrics header (no editor, instant)
      --texture-only  Only import the textures (leave the header alone)
      --weight NAME   Import only this weight (Light | Bold | Hud). The header still
                      describes every weight. Use it to add or refresh ONE sheet
                      without rewriting an .uasset you have not locked — .uasset
                      is lockable in .gitattributes, so it is read-only until
                      Scripts/lock.sh says otherwise.
  -n, --dry-run       Print what would run; run nothing
  -h, --help          This text

AFTER RUNNING
  ./Scripts/build.sh
  git status Content/Trace/UI/Fonts Source/Trace/UI/Text

IN GAME
  Trace.Text.Report            names the face and weights that are actually drawing
  Trace.Text.Preview           specimen through BOTH renderers, in EVERY face
  -TraceNoFontAtlas            forces the Lato fallback, on purpose
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --header-only)  DO_EDITOR=0 ;;
        --texture-only) DO_HEADER=0 ;;
        --weight)       shift; [ $# -gt 0 ] || trace_die "--weight needs a name (Light | Bold | Hud)"
                        ONLY_WEIGHT="$1" ;;
        -n|--dry-run)   DRY_RUN=1 ;;
        -h|--help)      usage; exit 0 ;;
        *) trace_err "Unknown option: $1"; echo; usage; exit 2 ;;
    esac
    shift
done

trace_require_uproject

SRC_DIR="${TRACE_PROJECT_ROOT}/Content/Trace/UI/Fonts/Source"

# The weights and the REAL FONT FILE each one is rasterised from. Keep in step with
# the WEIGHTS table in Scripts/import_font_atlas.py and ETraceTextWeight in
# Source/Trace/UI/Text/TraceTextWeight.h.
#
# There is deliberately no --thin here. It used to say 4 for the light sheet, from
# when the light cut was synthesised by eroding Regular; regenerating that way now
# would put the damaged letterforms back (spec v23 §A3 and v25 §4: "Do NOT use
# --thin" — the second attempt deleted ( ) [ ] { } and / while the HUD draws "[E]").
#
# APPEND to these arrays, never insert: the index is the ETraceTextWeight value.
ATLASES=("T_FontAtlas" "T_FontAtlasBold" "T_FontAtlasHud")
WEIGHT_NAMES=("Light" "Bold" "Hud")
FONT_FILES=("Sofachrome W05 ExtraLight.ttf" "Sofachrome Rg.otf" "Erbaum-Bold.otf")

for I in "${!ATLASES[@]}"; do
    NAME="${ATLASES[$I]}"
    for F in "${SRC_DIR}/${NAME}.png" "${SRC_DIR}/${NAME}.json"; do
        [ -f "$F" ] || trace_die "Missing ${F}
Generate it first:  python3 Scripts/generate_font_atlas.py --name ${NAME} \\
    --font \"Art/Fonts/${FONT_FILES[$I]}\" --preview
That needs your own licensed copy of the face at Art/Fonts/ — see docs/FONTS.md."
    done

    # An LFS pointer is a ~130-byte text file starting with 'version https://'. Handing
    # that to the importer produces a baffling 'not a valid image' instead of a useful error.
    if head -c 16 "${SRC_DIR}/${NAME}.png" | grep -q '^version https'; then
        trace_die "${SRC_DIR}/${NAME}.png is an unfetched Git LFS pointer, not the sheet. Run: git lfs pull"
    fi
done

# ------------------------------------------------------------------------------
# 1. Metrics -> generated header
# ------------------------------------------------------------------------------
if [ "$DO_HEADER" = "1" ]; then
    trace_msg "Metrics  ${TRACE_C_BOLD}${#ATLASES[@]} weight(s)${TRACE_C_OFF} -> Source/Trace/UI/Text/TraceFontAtlasMetrics.h"
    if [ "$DRY_RUN" = "1" ]; then
        trace_print_cmd python3 "${TRACE_SCRIPT_DIR}/import_font_atlas.py"
    else
        python3 "${TRACE_SCRIPT_DIR}/import_font_atlas.py"
    fi
fi

if [ "$DO_EDITOR" != "1" ]; then
    trace_msg "--header-only: the texture was not imported."
    exit 0
fi

# ------------------------------------------------------------------------------
# 2. PNG -> UTexture2D, inside the editor
#
# -NullRHI is safe here and is NOT safe everywhere in this project: importing a
# texture builds no shader map, so unlike Scripts/import-railgun.sh (which
# creates materials) this needs no swap chain. It is also not a font import, so
# unlike Scripts/generate-menu-widgets.py it does not need a Slate application —
# a .ttf import calls FSlateApplication::Get() unconditionally and SIGSEGVs a
# commandlet; a .png import does not go near it.
# ------------------------------------------------------------------------------
trace_resolve_engine
CMD_BIN="$(trace_editor_cmd_binary)"

ARGS=("$TRACE_UPROJECT"
      -run=pythonscript
      "-script=${TRACE_SCRIPT_DIR}/import_font_atlas.py"
      -unattended
      -nosplash
      -nopause
      -nosound
      -NullRHI
      -stdout
      -FullStdOutLogOutput)

trace_msg "Textures ${TRACE_C_BOLD}${ATLASES[*]}${TRACE_C_OFF} -> /Game/Trace/UI/Fonts/"

if [ "$DRY_RUN" = "1" ]; then
    trace_print_cmd "$CMD_BIN" "${ARGS[@]}"
    exit 0
fi

# The header is already current from step 1, and re-deriving it inside the editor
# would need Pillow in the editor's interpreter (it does not have it) and would
# silently downgrade the measured cap height to an estimate.
export TRACE_SKIP_HEADER=1

# Narrows step 2 only; the header above always describes every weight.
if [ -n "$ONLY_WEIGHT" ]; then
    export TRACE_FONT_ATLAS_WEIGHTS="$ONLY_WEIGHT"
    trace_msg "--weight ${ONLY_WEIGHT}: only that sheet will be imported."
fi

# THE EXIT CODE OF THE COMMANDLET IS NOT THE RESULT OF THE RUN — it is non-zero if
# ANY error was logged in the whole session, including engine warnings raised at
# startup that have nothing to do with this. What reached disk, below, is the
# authoritative check. (Same reasoning as Scripts/import-railgun.sh.)
set +e
"$CMD_BIN" "${ARGS[@]}" 2>&1 | grep -E '\[Trace\]|LogPythonScriptCommandlet' || true
set -e

# ------------------------------------------------------------------------------
# 3. Verify what landed
# ------------------------------------------------------------------------------
EXPECTED=("Source/Trace/UI/Text/TraceFontAtlasMetrics.h")
for I in "${!ATLASES[@]}"; do
    # With --weight, the other sheets were deliberately not touched this run, so
    # only the one that was asked for is evidence of anything.
    if [ -z "$ONLY_WEIGHT" ] || [ "$ONLY_WEIGHT" = "${WEIGHT_NAMES[$I]}" ]; then
        EXPECTED+=("Content/Trace/UI/Fonts/${ATLASES[$I]}.uasset")
    fi
done

MISSING=0
trace_msg "Verifying:"
for Rel in "${EXPECTED[@]}"; do
    if [ -f "${TRACE_PROJECT_ROOT}/${Rel}" ]; then
        printf '    ok      %s\n' "$Rel"
    else
        printf '    MISSING %s\n' "$Rel"
        MISSING=$((MISSING + 1))
    fi
done

if [ "$MISSING" != "0" ]; then
    trace_err "${MISSING} output(s) did not land. Search the output above for '[Trace]' lines."
    trace_err "If the editor is already open on this project, close it and re-run — two processes"
    trace_err "cannot both write Content/Trace/UI/Fonts."
    exit 1
fi

trace_msg "The atlas is imported (${#ATLASES[@]} faces: ${ATLASES[*]})."
trace_msg "Next: ./Scripts/build.sh, then in game: Trace.Text.Report / Trace.Text.Preview"
