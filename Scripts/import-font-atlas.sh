#!/usr/bin/env bash
# ==============================================================================
# Trace — import-font-atlas.sh
#
# Puts SOFACHROME on screen. Runs the two halves of Scripts/import_font_atlas.py:
#
#   1. metrics  Content/Trace/UI/Fonts/Source/T_FontAtlas.json
#                 -> Source/Trace/UI/Text/TraceFontAtlasMetrics.h    (plain python)
#   2. texture  Content/Trace/UI/Fonts/Source/T_FontAtlas.png
#                 -> Content/Trace/UI/Fonts/T_FontAtlas.uasset       (inside the editor)
#
# YOU DO NOT NEED TO RUN THIS TO PLAY. Both outputs are committed, exactly like
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
# Sofachrome is licensed to the owner for desktop use, not for embedding, and
# this repo is public. Only the rasterised sheet is committed. Regenerating it
# needs your own licensed copy at Art/Fonts/Sofachrome Rg.otf (gitignored) —
# see docs/FONTS.md and the header of Scripts/generate_font_atlas.py, which also
# records that a whole-charset atlas is a GREY position that a commercial
# release should replace with a bought app licence.
# ==============================================================================
set -euo pipefail

. "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)/_trace_common.sh"

DO_EDITOR=1
DO_HEADER=1
DRY_RUN=0

usage() {
    cat <<EOF
${TRACE_PROJECT_NAME} import-font-atlas

Regenerates Source/Trace/UI/Text/TraceFontAtlasMetrics.h and imports
Content/Trace/UI/Fonts/T_FontAtlas from the sheet under .../Fonts/Source.

USAGE
  Scripts/import-font-atlas.sh [options]

OPTIONS
      --header-only   Only regenerate the metrics header (no editor, instant)
      --texture-only  Only import the texture (leave the header alone)
  -n, --dry-run       Print what would run; run nothing
  -h, --help          This text

AFTER RUNNING
  ./Scripts/build.sh
  git status Content/Trace/UI/Fonts Source/Trace/UI/Text

IN GAME
  Trace.Text.Report            names the face that is actually drawing
  Trace.Text.Preview           puts a specimen on screen through BOTH renderers
  -TraceNoFontAtlas            forces the Lato fallback, on purpose
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --header-only)  DO_EDITOR=0 ;;
        --texture-only) DO_HEADER=0 ;;
        -n|--dry-run)   DRY_RUN=1 ;;
        -h|--help)      usage; exit 0 ;;
        *) trace_err "Unknown option: $1"; echo; usage; exit 2 ;;
    esac
    shift
done

trace_require_uproject

SRC_DIR="${TRACE_PROJECT_ROOT}/Content/Trace/UI/Fonts/Source"
PNG="${SRC_DIR}/T_FontAtlas.png"
JSON="${SRC_DIR}/T_FontAtlas.json"

for F in "$PNG" "$JSON"; do
    [ -f "$F" ] || trace_die "Missing ${F}
Generate it first:  python3 Scripts/generate_font_atlas.py --preview
That needs your own licensed Sofachrome at Art/Fonts/ — see docs/FONTS.md."
done

# An LFS pointer is a ~130-byte text file starting with 'version https://'. Handing
# that to the importer produces a baffling 'not a valid image' instead of a useful error.
if head -c 16 "$PNG" | grep -q '^version https'; then
    trace_die "${PNG} is an unfetched Git LFS pointer, not the sheet. Run: git lfs pull"
fi

# ------------------------------------------------------------------------------
# 1. Metrics -> generated header
# ------------------------------------------------------------------------------
if [ "$DO_HEADER" = "1" ]; then
    trace_msg "Metrics  ${TRACE_C_BOLD}T_FontAtlas.json${TRACE_C_OFF} -> Source/Trace/UI/Text/TraceFontAtlasMetrics.h"
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

trace_msg "Texture  ${TRACE_C_BOLD}T_FontAtlas.png${TRACE_C_OFF} -> /Game/Trace/UI/Fonts/T_FontAtlas"

if [ "$DRY_RUN" = "1" ]; then
    trace_print_cmd "$CMD_BIN" "${ARGS[@]}"
    exit 0
fi

# The header is already current from step 1, and re-deriving it inside the editor
# would need Pillow in the editor's interpreter (it does not have it) and would
# silently downgrade the measured cap height to an estimate.
export TRACE_SKIP_HEADER=1

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
EXPECTED=(
    "Content/Trace/UI/Fonts/T_FontAtlas.uasset"
    "Source/Trace/UI/Text/TraceFontAtlasMetrics.h"
)

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

trace_msg "Sofachrome is imported."
trace_msg "Next: ./Scripts/build.sh, then in game: Trace.Text.Report / Trace.Text.Preview"
