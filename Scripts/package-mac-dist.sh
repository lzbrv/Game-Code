#!/usr/bin/env bash
# ==============================================================================
# Trace — package-mac-dist.sh
#
# Turns a built .app into THE THING YOU ACTUALLY SEND: one zip containing the
# app and a README that tells the recipient how to open it.
#
# WHY THIS EXISTS
# ===============
# Scripts/package.sh produces a bundle that runs perfectly on the machine that
# built it and DOES NOT OPEN on anybody else's Mac. That is not a build failure
# and no amount of rebuilding fixes it:
#
#   * The bundle is AD-HOC SIGNED (codesign reports "Signature=adhoc",
#     "TeamIdentifier=not set"). Ad-hoc is a real, valid signature — it just has
#     no certificate chain, so Gatekeeper cannot attribute it to anyone.
#   * Anything a browser, Messages, AirDrop or Slack hands to a Mac gets the
#     com.apple.quarantine extended attribute.
#   * Quarantine + no Developer ID = macOS refuses to launch it, often with
#     "Trace is damaged and can't be opened. You should move it to the Trash",
#     which reads exactly like a corrupt download and is not one.
#
# MEASURED, not assumed. On macOS 26.5, with the real Finder path (Safari-style
# quarantine on the zip, expanded by Archive Utility, launched with `open`):
#
#     quarantined copy      -> 0 processes after 15s.  It does not start.
#     quarantine cleared    -> 1 process after 15s.    It starts.
#
# The remedy has always been in docs/PLAYTEST.md and the owner's playtesters
# still hit the wall, which means a line in a doc nobody opens is not a remedy.
# So the instruction now ships INSIDE THE ZIP, next to the app, in a file whose
# name is the instruction. This script is what guarantees that — the artefact is
# assembled by the build, not by somebody remembering to assemble it.
#
# WHAT AN APPLE DEVELOPER ACCOUNT WOULD BUY (the owner is assumed not to have
# one): USD 99/year for a Developer ID Application certificate. With it,
# `codesign --sign "Developer ID Application: ..." --options runtime` plus
# `xcrun notarytool submit --wait` plus `xcrun stapler staple` produces a bundle
# that opens on a double-click with NO instructions and NO Terminal, on every
# Mac, forever. That is the only thing that removes this whole file's reason to
# exist. Nothing below is a substitute for it; it is the best that is reachable
# without it.
# ==============================================================================
set -euo pipefail

. "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)/_trace_common.sh"

APP=""
OUTPUT=""
DO_LAUNCH_PROOF=1
DO_QUARANTINE_PROOF=0

DEFAULT_OUTPUT="${TRACE_PROJECT_ROOT}/Saved/Packaged/Mac"

usage() {
    cat <<EOF
${TRACE_PROJECT_NAME} package-mac-dist — build the sendable zip (app + README)

USAGE
  Scripts/package-mac-dist.sh [options]

OPTIONS
  -a, --app <path>        The .app to package. Default: the single .app under
                          <output>, preferring ${TRACE_PROJECT_NAME}-Mac-Shipping.app
  -o, --output <dir>      Where the app is and where the zip goes.
                          Default: ${DEFAULT_OUTPUT}
      --no-launch-proof   Skip launching the finished artefact. Faster; proves less.
      --prove-quarantine  ALSO run the failing arm: quarantine a copy exactly the
                          way a download does and show that it does NOT start.
                          Off by default because it deliberately trips Gatekeeper
                          and macOS may put a "damaged" dialog on your screen —
                          which is the point, but is rude to do unasked.
  -h, --help              This text

WHAT COMES OUT
  <output>/${TRACE_PROJECT_NAME}-Mac-<Config>.zip     <- send this, nothing else
  <output>/${TRACE_PROJECT_NAME}-Mac/                 <- the staging folder it was made from
  <output>/SEND-THIS-MESSAGE.txt                <- paste this into the chat message

  The zip expands to a folder holding the app and a README named
  "READ ME FIRST - Trace will not open until you do this.txt". A recipient who
  reads nothing at all still sees that filename.
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        -a|--app)             [ $# -ge 2 ] || trace_die "--app needs a value"; APP="$2"; shift 2 ;;
        -o|--output)          [ $# -ge 2 ] || trace_die "--output needs a value"; OUTPUT="$2"; shift 2 ;;
        --no-launch-proof)    DO_LAUNCH_PROOF=0; shift ;;
        --prove-quarantine)   DO_QUARANTINE_PROOF=1; shift ;;
        -h|--help)            usage; exit 0 ;;
        *)                    trace_err "Unknown argument: $1"; echo; usage; exit 2 ;;
    esac
done

[ "$TRACE_HOST_PLATFORM" = "Mac" ] || trace_die "This script only makes sense on macOS."
[ -n "$OUTPUT" ] || OUTPUT="$DEFAULT_OUTPUT"

# ------------------------------------------------------------------------------
# Find the bundle. Same resolution order as package.sh, and for the same reason:
# UBT names a game bundle after its target receipt, so only Development gets the
# bare Trace.app and every other config carries the platform and configuration.
# ------------------------------------------------------------------------------
if [ -z "$APP" ]; then
    for Candidate in \
        "${OUTPUT}/${TRACE_PROJECT_NAME}-Mac-Shipping.app" \
        "${OUTPUT}/${TRACE_PROJECT_NAME}-Mac-Test.app" \
        "${OUTPUT}/${TRACE_PROJECT_NAME}.app"; do
        if [ -d "$Candidate" ]; then APP="$Candidate"; break; fi
    done
fi
if [ -z "$APP" ]; then
    APP="$(find "$OUTPUT" -maxdepth 1 -name '*.app' 2>/dev/null | head -1)"
fi
[ -n "$APP" ] && [ -d "$APP" ] || trace_die "No .app found under ${OUTPUT}. Run Scripts/package.sh first."

APP_NAME="$(basename "$APP")"
APP_STEM="${APP_NAME%.app}"
EXE="${APP}/Contents/MacOS/${APP_STEM}"
[ -x "$EXE" ] || trace_die "No executable at ${EXE} — that bundle is not a runnable app."

trace_msg "Distributable for ${TRACE_C_BOLD}${APP_NAME}${TRACE_C_OFF}"

# ==============================================================================
# THE GATES.
#
# Every one of these is a thing that makes the artefact fail on somebody else's
# machine in a way that does NOT look like its own cause. They are checked here,
# where the message can still be useful, rather than by a friend at 9pm.
# ==============================================================================

FAILURES=0
fail() { trace_err "$*"; FAILURES=$((FAILURES + 1)); }

# ---- GATE A — the signature must be VALID -------------------------------------
#
# Ad-hoc is fine and expected. BROKEN is not, and the two produce the SAME
# "damaged" dialog on the recipient's Mac, which is why they have to be told
# apart here. A broken signature is usually a zip that mangled a symlink or a
# file edited inside the bundle after signing, and its fix (re-sign, or re-zip
# with ditto) is completely different from quarantine's.
trace_msg "Gate A: code signature"
if ! codesign --verify --deep --strict --verbose=2 "$APP" >/dev/null 2>&1; then
    fail "The code signature is BROKEN, not merely ad-hoc."
    codesign --verify --deep --strict --verbose=2 "$APP" 2>&1 | head -10 >&2 || true
    trace_err "  A recipient sees 'is damaged and can't be opened' for this reason too, and"
    trace_err "  clearing quarantine will NOT fix it. Re-package before sending anything."
else
    SIGN_AUTHORITY="$(codesign -dvv "$APP" 2>&1 | sed -n 's/^Authority=//p' | head -1)"
    SIGN_FLAGS="$(codesign -dvv "$APP" 2>&1 | sed -n 's/^Signature=//p' | head -1)"
    if [ -n "$SIGN_AUTHORITY" ]; then
        trace_msg "  valid, signed by: ${SIGN_AUTHORITY}"
    else
        trace_msg "  valid, ad-hoc (${SIGN_FLAGS:-adhoc}). Expected without a Developer ID."
    fi
fi

# ---- GATE B — the sandbox must allow the network ------------------------------
#
# Config/DefaultEngine.ini points ShippingSpecificMacEntitlements at the engine's
# Sandbox.Server.entitlements for a measured reason (that block is worth reading:
# the default Shipping entitlements have NO network at all and the game reports a
# busy port when nothing is on it). This gate is what stops that regressing
# silently, because the symptom is a game that runs and cannot host.
trace_msg "Gate B: sandbox entitlements"
ENTS="$(codesign -d --entitlements - --xml "$APP" 2>/dev/null || true)"
if [ -z "$ENTS" ]; then
    fail "The bundle has NO entitlements at all. Under App Sandbox it cannot open a socket."
else
    MISSING_ENTS=""
    for Key in com.apple.security.network.client com.apple.security.network.server; do
        case "$ENTS" in
            *"$Key"*) : ;;
            *) MISSING_ENTS="${MISSING_ENTS} ${Key}" ;;
        esac
    done
    case "$ENTS" in
        *com.apple.security.app-sandbox*) SANDBOXED=1 ;;
        *) SANDBOXED=0 ;;
    esac
    if [ -n "$MISSING_ENTS" ] && [ "$SANDBOXED" = "1" ]; then
        fail "Sandboxed with no network entitlement:${MISSING_ENTS}"
        trace_err "  The kernel will refuse the UDP bind. The game will run, report"
        trace_err "  'COULD NOT LISTEN ON UDP 7777' and be unjoinable, with nothing on the port."
        trace_err "  Fix: ShippingSpecificMacEntitlements in Config/DefaultEngine.ini."
    elif [ -n "$MISSING_ENTS" ]; then
        trace_msg "  not sandboxed; network entitlements not required."
    else
        trace_msg "  sandboxed WITH network.client + network.server. Hosting is possible."
    fi
fi

# ---- GATE C — a bundle identifier that is not Epic's placeholder --------------
trace_msg "Gate C: bundle identifier"
BUNDLE_ID="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "${APP}/Contents/Info.plist" 2>/dev/null || echo '')"
case "$BUNDLE_ID" in
    ""|com.YourCompany.*)
        fail "CFBundleIdentifier is '${BUNDLE_ID:-<missing>}' — Epic's placeholder."
        trace_err "  Every un-renamed UE project ships as com.YourCompany.<Project>, so macOS"
        trace_err "  cannot tell this game from any other. Set BundleIdentifier under"
        trace_err "  [/Script/MacTargetPlatform.XcodeProjectSettings] in Config/DefaultEngine.ini."
        ;;
    *) trace_msg "  ${BUNDLE_ID}" ;;
esac

# ---- GATE D — architectures, reported rather than judged ----------------------
#
# NOT a failure: arm64-only is a legitimate choice and universal costs build time
# and roughly doubles the download. It is reported because an Intel Mac cannot
# run an arm64 build AT ALL, Rosetta does not help (it translates Intel for Apple
# Silicon, not the reverse), and the symptom is yet another unhelpful dialog.
trace_msg "Gate D: architectures"
ARCHS="$(lipo -archs "$EXE" 2>/dev/null || echo unknown)"
case "$ARCHS" in
    *x86_64*arm64*|*arm64*x86_64*) trace_msg "  ${ARCHS} — universal. Any Mac from the last decade can run it." ;;
    *arm64*)
        trace_msg "  ${ARCHS} — Apple Silicon ONLY."
        trace_msg "  An Intel Mac cannot run this. If anyone in the group has one, rebuild with:"
        trace_msg "      Scripts/package.sh -- -architecture=arm64+x86_64"
        trace_msg "  (roughly double the build time and a bigger download; nothing else changes.)"
        ;;
    *) trace_msg "  ${ARCHS}" ;;
esac

# ---- GATE E — cooked content, restated -----------------------------------------
#
# package.sh already gates on this; repeated here because this script can be run
# on its own against a bundle from any source, and a bundle with no cooked
# content exits instantly at launch with no message at all.
PAK_COUNT="$(find "$APP" \( -name '*.pak' -o -name '*.utoc' \) 2>/dev/null | wc -l | tr -d ' ')"
UASSET_COUNT="$(find "$APP" -name '*.uasset' 2>/dev/null | wc -l | tr -d ' ')"
if [ "${PAK_COUNT:-0}" = "0" ] && [ "${UASSET_COUNT:-0}" = "0" ]; then
    fail "No cooked content in the bundle. It will exit immediately at launch."
fi

if [ "$FAILURES" -gt 0 ]; then
    trace_die "${FAILURES} gate(s) failed. Not building a distributable from a bundle that will not work."
fi

# ==============================================================================
# STAGE AND ZIP
# ==============================================================================

NET_VERSION="$(python3 "${TRACE_SCRIPT_DIR}/netversion.py" --quiet 2>/dev/null || echo 'NET unknown')"
APP_SIZE="$(du -sh "$APP" 2>/dev/null | cut -f1 | tr -d ' ')"

STAGE="${OUTPUT}/${TRACE_PROJECT_NAME}-Mac"
ZIP="${OUTPUT}/${APP_STEM}.zip"
README="${STAGE}/READ ME FIRST - Trace will not open until you do this.txt"

rm -rf "$STAGE" "$ZIP"
mkdir -p "$STAGE"

# ditto, not cp: it is the only copy on macOS that reliably preserves the
# extended attributes, ACLs and symlinks a code signature is sealed over. `cp -R`
# has broken app signatures on this platform for years.
trace_msg "Staging ${STAGE}"
ditto "$APP" "${STAGE}/${APP_NAME}"

cat > "$README" <<EOF
================================================================================
  T R A C E  —  macOS
  IT WILL NOT OPEN UNTIL YOU DO THE ONE THING BELOW. That is not a bug and the
  download is not broken.
================================================================================

WHAT YOU WILL SEE IF YOU SKIP THIS
----------------------------------
You double-click ${APP_NAME} and macOS says one of:

    "Trace is damaged and can't be opened. You should move it to the Trash."
    "Apple could not verify Trace is free of malware."
    (or simply nothing happens at all)

NOTHING IS DAMAGED. Do not delete it and ask for another copy — you will get an
identical result. macOS says "damaged" when it means "this app was not signed by
a developer who paid Apple \$99 a year". Trace IS signed, just not by one of
those, and macOS cannot tell those two situations apart. Every Mac does this to
every app that did not come from the App Store or from a paid developer.


THE FIX — AND *WHEN* YOU DO IT DECIDES WHICH ONE YOU NEED
---------------------------------------------------------
Read this bit before you type anything. It is short and it saves a lot of
confusion.

macOS locks an app bundle against being modified once it has DECIDED not to run
it. So the famous one-line fix works perfectly right up until you double-click
the app — and stops working the moment you do. Both cases are below. Pick yours.


CASE 1 — YOU HAVE NOT DOUBLE-CLICKED THE APP YET (the easy case)
---------------------------------------------------------------
Do it to the app, in Terminal:

  1. Open Terminal.  (Cmd+Space, type: terminal, press Return.)
  2. Type this INCLUDING THE TRAILING SPACE, do NOT press Return yet:

         xattr -dr com.apple.quarantine

  3. Drag ${APP_NAME} from this folder into the Terminal window — that fills in
     the path — and NOW press Return.
  4. Double-click ${APP_NAME}. It starts.

No output means it worked. If instead you see a wall of
"Operation not permitted", you are in Case 2.


CASE 2 — YOU ALREADY DOUBLE-CLICKED IT AND GOT "DAMAGED"
--------------------------------------------------------
This is most people, because nobody reads a README until something fails. The
command above will now refuse with "Operation not permitted" on every file, and
sudo does not help — it is not that kind of permission.

Make a clean copy instead. This is one line, it always works, and it does not
touch the blocked copy at all:

    ditto --noextattr --noqtn "<drag the app here>" ~/Trace.app

Then double-click ~/Trace.app (it will be in your home folder, the one with the
house icon). That copy starts normally. Delete the blocked one whenever you like.

If you would rather start over: throw away this whole unzipped folder, then do
the ZIP fix below on the .zip you downloaded and unzip it again.


THE ZIP FIX — the one to use if you still have the .zip
-------------------------------------------------------
A zip is an ordinary file, so none of the app-bundle protections apply to it.
Clear the flag on the ZIP and everything that comes out of it is already clean:

    xattr -d com.apple.quarantine "<drag the .zip here>"      (no "r")

Then double-click the zip and run the app out of the folder that appears. If you
are the person SENDING this build, tell people to do this first and neither case
above ever comes up.


THE WAY THAT AVOIDS ALL OF THIS
-------------------------------
The quarantine flag is attached by whatever DOWNLOADS the file — a browser,
Messages, AirDrop, Slack, Discord. It is not attached by the network and not by
the file itself. So a copy that arrives any other way never has it and never
needs any of the above:

    curl -L -o ~/Downloads/${APP_STEM}.zip "<the link>"
    scp  someone@100.x.x.x:${APP_STEM}.zip ~/Downloads/

If you are already on the Tailscale network in order to play, scp over the
tailnet is the easiest route and skips this whole page.


THE "Unlock Trace (double-click me).command" FILE
-------------------------------------------------
It is next to this README and it does all of the above for you: it tries the
simple fix, and if the app is already locked it makes the clean copy instead.
Finder may warn before running it, because it is an unsigned script from the
internet too. If that gets in your way, the typed commands above always work.


BEFORE YOU CAN PLAY: TWO THINGS TO CHECK
----------------------------------------
1. APPLE SILICON ONLY.  This build is ${ARCHS}. An Intel Mac cannot run it and
   Rosetta does not help — Rosetta translates Intel code to run on Apple
   Silicon, not the other way round. Check with  > About This Mac: you need
   "Chip: Apple M-something", not "Processor: Intel".

2. YOUR BUILD MUST MATCH THE HOST'S.  On the title screen, bottom right, this
   build shows:

       ${NET_VERSION}

   Everyone in the session must see that SAME code. Windows players see it in
   the same corner. If two people see different codes they cannot connect, no
   matter how good the network is, and the error message will blame the
   connection. Compare the codes first — it takes five seconds.


PLAYING
-------
* PLAY also HOSTS. Whoever presses PLAY is the server; everyone else joins them.
* The host's address is printed on their own title screen. It is a Tailscale
  address (100.x.x.x) when Tailscale is up.
* Everyone else: JOIN, type that address, connect. The port is 7777/UDP and you
  do not need to type it.
* macOS will ask "Do you want the application Trace to accept incoming network
  connections?" the first time you host. Say Allow. Saying Deny makes you
  unjoinable with no other symptom.

--------------------------------------------------------------------------------
Build: ${APP_STEM}   ${APP_SIZE}   ${ARCHS}   ${NET_VERSION}
Bundle id: ${BUNDLE_ID}
Made by Scripts/package-mac-dist.sh — if this file is wrong, fix it there.
--------------------------------------------------------------------------------
EOF

# A second copy of the one command, as a double-clickable file, for anyone who
# will not open Terminal. It is honest about being a fallback: a script carries
# quarantine too, and Finder may warn before running it. The warning for a SCRIPT
# is the soft one with an Open button, not the hard "damaged" refusal an app
# gets, which is why this is worth shipping at all.
UNLOCK="${STAGE}/Unlock Trace (double-click me).command"
cat > "$UNLOCK" <<'UNLOCKEOF'
#!/bin/bash
# ---------------------------------------------------------------------------
# Trace — unlock helper.
#
# macOS quarantines anything you download and refuses to run an app that is
# quarantined and not signed by a paid Apple developer. This clears that.
#
# IT HANDLES BOTH STATES, WHICH IS THE WHOLE REASON IT EXISTS:
#   * If you have NOT double-clicked the app yet, clearing the flag works and
#     that is all this does.
#   * If you HAVE double-clicked it and got "damaged", macOS has locked the
#     bundle against modification and clearing the flag now fails with
#     "Operation not permitted" (sudo does not help). In that case this makes a
#     clean copy in your home folder instead, which starts normally.
# ---------------------------------------------------------------------------
cd "$(dirname "$0")" || exit 1
shopt -s nullglob
APPS=(*.app)

finish() { echo; echo "Press Return to close this window."; read -r _; exit "${1:-0}"; }

if [ ${#APPS[@]} -eq 0 ]; then
    echo "There is no .app next to this script."
    echo "Put this file in the same folder as the Trace app and run it again."
    finish 1
fi

for APP in "${APPS[@]}"; do
    echo "Unlocking ${APP} ..."
    xattr -dr com.apple.quarantine "$APP" >/dev/null 2>&1

    # DO NOT TRUST THE EXIT CODE. `xattr -dr` reports failure per file and its
    # overall status has been unreliable; the only honest answer is to look at
    # whether any quarantine flag is still there.
    REMAINING="$(xattr -r -l "$APP" 2>/dev/null | grep -c com.apple.quarantine)"

    if [ "${REMAINING:-0}" -eq 0 ]; then
        echo "  done — double-click ${APP} and it will start."
        continue
    fi

    echo "  macOS refused (this app has already been double-clicked once, which"
    echo "  locks it). Making a clean copy instead — this takes a minute."

    DEST="$HOME/${APP}"
    if [ -e "$DEST" ]; then
        DEST="$HOME/$(date +%Y%m%d-%H%M%S)-${APP}"
    fi

    if ditto --noextattr --noqtn "$APP" "$DEST"; then
        echo "  done."
        echo
        echo "  USE THIS COPY:  ${DEST}"
        echo "  It is in your home folder (the one with the house icon in Finder)."
        echo "  You can delete the copy in this folder whenever you like."
        open -R "$DEST" 2>/dev/null || true
    else
        echo "  that did not work either. Delete this whole unzipped folder, then run"
        echo "      xattr -d com.apple.quarantine <the .zip you downloaded>"
        echo "  and unzip it again. That route cannot hit this problem."
    fi
done

finish 0
UNLOCKEOF
chmod +x "$UNLOCK"

trace_msg "Zipping ${TRACE_C_BOLD}${ZIP}${TRACE_C_OFF} (this takes a minute)"
# --keepParent so the zip expands to a FOLDER containing the app and the README,
# rather than spraying both into whatever directory the recipient is standing in.
# --sequesterRsrc is what Finder's own "Compress" uses and is what keeps the code
# signature intact through the round trip.
ditto -c -k --sequesterRsrc --keepParent "$STAGE" "$ZIP"

ZIP_SIZE="$(du -sh "$ZIP" 2>/dev/null | cut -f1 | tr -d ' ')"
trace_msg "Zip: ${ZIP_SIZE}   (expands to ${APP_SIZE})"

# ------------------------------------------------------------------------------
# THE MESSAGE TO PASTE INTO CHAT — the one instruction the README cannot deliver.
#
# The README ships INSIDE the zip, which means a recipient can only read it after
# unzipping. That is fine for the fix (which the README leads with, aimed at
# exactly that moment) and useless for the ONE instruction that has to arrive
# BEFORE the download: "grab it with curl/scp and none of this happens".
#
# So the sending half gets its own artefact, deliberately NOT inside the zip,
# written to be copy-pasted into Discord or iMessage as-is.
# ------------------------------------------------------------------------------
MESSAGE="${OUTPUT}/SEND-THIS-MESSAGE.txt"
cat > "$MESSAGE" <<MSGEOF
Trace — Mac build. ${ZIP_SIZE} zipped, Apple Silicon only (M1 or later; an Intel Mac cannot run it).

macOS WILL refuse to open it the first time and will probably say "Trace is
damaged and can't be opened". It is not damaged — that is just what macOS says
about any app not signed by a paid Apple developer. There is a README inside the
zip, and this is the short version:

  DO THIS TO THE .ZIP BEFORE YOU UNZIP IT. Open Terminal, type this with a
  trailing space, then drag the .zip in from Finder and press Return:

      xattr -d com.apple.quarantine

  Then unzip and the app just opens. There is a longer README inside the zip.

  IF YOU ALREADY DOUBLE-CLICKED THE APP AND SAW "damaged": macOS has now locked
  that copy and the command above will refuse it. Run this instead and use the
  copy it makes:

      ditto --noextattr --noqtn <drag the app here> ~/Trace.app

Want to skip all of that? Grab it with curl instead of a browser — the
quarantine flag comes from the browser, not from the file:
      curl -L -o ~/Downloads/${APP_STEM}.zip "<paste the link here>"

Before we start: your title screen shows "${NET_VERSION}" in the bottom right
corner. Mine has to say the same thing or we cannot connect. Windows players see
it in the same corner.

PLAY also hosts. Whoever presses PLAY is the server; everyone else picks JOIN and
types that person's address (it is on their title screen). Port 7777/UDP. Say
"Allow" to the macOS / Windows firewall prompt the first time you host.
MSGEOF


# ==============================================================================
# PROVE IT. A ZIP THAT WAS NEVER OPENED IS NOT A TESTED ARTEFACT.
# ==============================================================================

PROOF_DIR=""

# Kill anything this proof started. TWO patterns, and the second is not optional:
# a QUARANTINED bundle is not run from where you launched it — macOS App
# Translocation copies it to /private/var/folders/.../AppTranslocation/<uuid>/d/
# and runs it there, so a PROOF_DIR-anchored pkill leaves it behind. That is how
# a stray game process outlived this script and was found running an hour later.
# EVERY line here needs its own `|| true`. This script runs under `set -e`, pkill
# returns 1 when it matches nothing, and "nothing was left running" is the normal
# case — so an unguarded pkill turns a SUCCESSFUL package into a failed one.
# (It did: package.sh calls this script under `set -e`, and a first cut of this
# function without the guards made the whole packaging run exit 1 after writing a
# perfectly good zip.)
kill_proof_processes() {
    if [ -n "$PROOF_DIR" ]; then
        pkill -f "${PROOF_DIR}/" >/dev/null 2>&1 || true
    fi
    pkill -f "AppTranslocation.*/${APP_STEM}.app/" >/dev/null 2>&1 || true
    return 0
}
cleanup_proof() {
    kill_proof_processes
    if [ -n "$PROOF_DIR" ] && [ -d "$PROOF_DIR" ]; then rm -rf "$PROOF_DIR"; fi
}
trap cleanup_proof EXIT

# ------------------------------------------------------------------------------
# *** HOW "DID IT OPEN?" IS MEASURED, AND WHY IT IS NOT A PROCESS COUNT. ***
#
# `open` returns 0 whether or not Gatekeeper allowed the launch, so its exit code
# proves nothing. The obvious replacement — count processes under the bundle you
# launched — is WRONG IN BOTH DIRECTIONS and this script shipped it:
#
#   1. It cannot see a refused launch at all. A quarantined bundle is App-
#      Translocated, so it runs from a path that has nothing to do with the one
#      you passed in, and a path-anchored grep returns 0 forever. The arm below
#      would have printed "quarantined copy did NOT start" on a machine where it
#      started perfectly well: a check that cannot fail, which is worth nothing.
#   2. A process EXISTING is not the game running. The refused launch does leave
#      a process behind — it just never executes anything.
#
# CPU TIME SEPARATES THEM AND NOTHING ELSE HERE DOES. Measured on this bundle,
# same zip, same folder, one minute each, quarantine the only variable:
#
#     quarantined   -> process exists, translocated, 0:00.01 s of CPU, 0.0 %
#     unquarantined -> process at the launch path,   1:15.91 s of CPU, ~115 %
#
# Four orders of magnitude. A UE game burns tens of CPU-seconds just opening its
# paks and compiling shaders; a bundle macOS refused burns a hundredth of one.
# ------------------------------------------------------------------------------

# Seconds of CPU accumulated by ANY process running this app, wherever it lives.
#
# All matching is done INSIDE awk and there is no grep in the pipeline, on purpose:
# under `set -o pipefail` a grep that matches nothing returns 1, and "nothing is
# running" is the ANSWER here, not an error. Written with grep, this function made
# the whole script exit 1 on a successful package — which `package.sh` runs under
# `set -e` and would have turned a good build into a failed one.
#
# ps prints "<cputime> <command>", so $2 is the first word of the command: exactly
# "awk" for this pipeline's own awk (which necessarily carries the search pattern in
# its arguments and would otherwise match itself) and an absolute path for the game.
proof_cpu_seconds() {
    ps ax -o time=,command= 2>/dev/null \
        | awk -v pat="MacOS/${APP_STEM}" '
            $2 == "awk" || $2 ~ /\/awk$/ { next }
            index($0, pat) == 0          { next }
            { n = split($1, t, ":"); s = (n == 3) ? t[1]*3600 + t[2]*60 + t[3] : t[1]*60 + t[2];
              if (s > m) m = s }
            END { printf "%d", m + 0 }'
}

# Echoes the CPU seconds the launch actually used. >= LAUNCH_CPU_THRESHOLD means
# the game came up; ~0 means a process exists and macOS never let it run.
LAUNCH_CPU_THRESHOLD=5
try_launch() {
    # $1 = .app path, $2 = seconds to wait
    kill_proof_processes
    sleep 1
    open -n "$1" --args -RenderOffScreen -nosplash >/dev/null 2>&1 || true
    sleep "$2"
    SECS="$(proof_cpu_seconds)"
    kill_proof_processes
    echo "${SECS:-0}"
}

if [ "$DO_LAUNCH_PROOF" = "1" ]; then
    PROOF_DIR="$(mktemp -d -t trace-dist-proof)"
    trace_msg "Proof: expanding the finished zip and launching what comes out"

    mkdir -p "${PROOF_DIR}/clean"
    ditto -x -k "$ZIP" "${PROOF_DIR}/clean"
    CLEAN_APP="${PROOF_DIR}/clean/${TRACE_PROJECT_NAME}-Mac/${APP_NAME}"
    [ -d "$CLEAN_APP" ] || trace_die "The zip did not expand to the expected layout: ${CLEAN_APP}"

    # The README must be where the README says it is.
    [ -f "${PROOF_DIR}/clean/${TRACE_PROJECT_NAME}-Mac/$(basename "$README")" ] \
        || trace_die "The README is not inside the zip. That is the whole point of this script."

    # Signature must have survived the zip round trip. If it did not, the recipient
    # gets "damaged" for a reason clearing quarantine cannot fix.
    codesign --verify --deep --strict "$CLEAN_APP" >/dev/null 2>&1 \
        || trace_die "The code signature did NOT survive the zip round trip. Do not send this."
    trace_msg "  signature survived the round trip"

    RUNNING="$(try_launch "$CLEAN_APP" 20)"
    if [ "$RUNNING" -ge "$LAUNCH_CPU_THRESHOLD" ]; then
        trace_msg "  ${TRACE_C_BOLD}unquarantined copy ran${TRACE_C_OFF} (${RUNNING}s of CPU in 20s). The artefact works."
    else
        trace_die "The unquarantined copy burned only ${RUNNING}s of CPU in 20s — it did not come up. This is a broken build, not a Gatekeeper problem."
    fi

    if [ "$DO_QUARANTINE_PROOF" = "1" ]; then
        # THE FAILING ARM. Without this, "unquarantined copies launch" is a check
        # that cannot fail and therefore proves nothing about quarantine at all.
        trace_msg "Proof: the same artefact, quarantined the way a download is"
        cp "$ZIP" "${PROOF_DIR}/dl.zip"
        xattr -w com.apple.quarantine "0083;$(printf %x "$(date +%s)");Safari;$(uuidgen)" "${PROOF_DIR}/dl.zip"
        mkdir -p "${PROOF_DIR}/dirty"
        ditto -x -k "${PROOF_DIR}/dl.zip" "${PROOF_DIR}/dirty"
        DIRTY_APP="${PROOF_DIR}/dirty/${TRACE_PROJECT_NAME}-Mac/${APP_NAME}"
        QFLAG="$(xattr -p com.apple.quarantine "$DIRTY_APP" 2>/dev/null || true)"
        [ -n "$QFLAG" ] || trace_die "ditto did not propagate quarantine — this proof is not testing what it claims."
        trace_msg "  quarantine on the expanded app: ${QFLAG}"

        BLOCKED="$(try_launch "$DIRTY_APP" 20)"
        if [ "$BLOCKED" -lt "$LAUNCH_CPU_THRESHOLD" ]; then
            trace_msg "  ${TRACE_C_BOLD}quarantined copy did NOT come up${TRACE_C_OFF} — reproduced the recipient's failure."
            trace_msg "  Both arms measured, same zip, quarantine the only variable:"
            trace_msg "    quarantined   ${BLOCKED}s of CPU in 20s"
            trace_msg "    unquarantined ${RUNNING}s of CPU in 20s"
        else
            trace_warn "  The quarantined copy RAN anyway (${BLOCKED}s of CPU in 20s)."
            trace_warn "  That means THIS Mac trusts this bundle already (it built it), so the"
            trace_warn "  failing arm cannot be demonstrated here. It will still fail elsewhere."
        fi
    fi
fi

trace_msg ""
trace_msg "${TRACE_C_BOLD}SEND THIS ONE FILE:${TRACE_C_OFF} ${ZIP}"
trace_msg "It contains the app and the unlock instructions. Do not send the .app on its own —"
trace_msg "the instructions are the half that makes it work."
trace_msg ""
trace_msg "PASTE THIS INTO THE CHAT MESSAGE ALONGSIDE IT:"
trace_msg "  ${MESSAGE}"
trace_msg "  (the README is inside the zip, so it cannot be read until after the download;"
trace_msg "   this file is the half that has to arrive first.)"
trace_msg "Compatibility code in this build: ${TRACE_C_BOLD}${NET_VERSION}${TRACE_C_OFF} (must match every other machine)"
