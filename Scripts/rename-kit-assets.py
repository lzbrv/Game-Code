# =============================================================================
# Trace - rename-kit-assets.py                 (release overhaul, MAP plan §10.4)
#
# GIVES THE COLLABORATOR'S FOUR CENTRE-KIT MESHES THE PROJECT'S OWN ASSET NAMES.
#
#   /Game/Trace/Art/Pack/MapGeometry/octagon   -> SM_KitOctagon
#                                    platform1 -> SM_KitPlatform
#                                    ramp1     -> SM_KitRamp
#                                    ramp      -> SM_KitRampAlt
#
# and removes the stray root duplicate /Game/ramp (content-inventory.md §1).
#
# -----------------------------------------------------------------------------
# WHY IT IS TWO PHASES, AND WHY THE RE-BAKE SITS BETWEEN THEM
# -----------------------------------------------------------------------------
# A rename leaves a UObjectRedirector at the old path, and the ONLY packages in
# this repo that name these meshes are Arena_Baked's One-File-Per-Actor packages
# for the eleven hand-placed kit actors (measured: `strings` over every .uasset
# and .umap in Content/ finds `/Game/Trace/Art/Pack/MapGeometry/<name>` in those
# eleven packages and nowhere else - not in the Fish maps, not in MainMenu, not
# in Arena.umap, not in any material or data asset).
#
# Those eleven packages are DELETED AND RE-EMITTED by the force bake that
# rebake-arena-preserving.py runs, and the restore re-spawns the kit actors from
# the census, resolving each mesh through this script's rename table. So:
#
#   phase `rename`   - rename the four assets. Redirectors appear. The map still
#                      loads: LoadObject follows a redirector, and the restore
#                      remaps the path explicitly anyway (MESH_RENAMES in
#                      rebake-arena-preserving.py, which quotes this file).
#   ...the re-bake runs...
#   phase `cleanup`  - by now the re-emitted packages name the NEW paths, so the
#                      redirectors are referenced by nothing. Verify that with
#                      the asset registry, then delete them, then delete
#                      /Game/ramp on the same evidence.
#
# THAT IS THE REDIRECTOR FIXUP, and it is stronger than the usual one.
# IAssetTools::FixupReferencers is NOT a UFUNCTION in UE 5.8
# (Engine/Source/Developer/AssetTools/Public/IAssetTools.h:658 - no UFUNCTION
# macro on it, unlike RenameAssets above it), so Python cannot call it here; the
# script tries it by name anyway and says which route it took. The regeneration
# route is what actually runs: rather than PATCHING the referencing packages,
# the bake rewrites them from the builder, so a referencer cannot be left half
# fixed up. What "clean" then means is checkable and is checked - zero
# referencers before each delete, and a `strings` sweep afterwards that must
# find the new names and none of the old ones.
#
# -----------------------------------------------------------------------------
# HOW TO RUN IT
# -----------------------------------------------------------------------------
#     "/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor" \
#         /path/to/Trace.uproject \
#         -run=pythonscript -script=".../Scripts/rename-kit-assets.py" \
#         -unattended -nosplash -nopause -stdout -FullStdOutLogOutput
#
#     env: TRACE_RENAME_PHASE = rename (default) | cleanup | report
#
# One editor instance, alarm-wrapped, and the working set writable first (LFS
# checks .uasset files out read-only):
#     chmod -R u+w Content/Trace/Art/Pack/MapGeometry Content/ramp.uasset
#
# IDEMPOTENT. `rename` skips a pair whose destination already exists; `cleanup`
# skips a path that is already gone. Running the whole thing twice is a no-op.
#
# Greppable evidence, all prefixed [RENAME]:
#   [RENAME][PLAN]      what each phase intends, before it does anything
#   [RENAME][MOVED]     one asset renamed, with the readback of the new path
#   [RENAME][REFS]      referencer count for a path about to be deleted
#   [RENAME][DELETED]   a redirector or the stray root duplicate, removed
#   [RENAME][KEPT]      something the script REFUSED to delete, and why
#   [RENAME][DONE]      the final census
#   [RENAME][FAIL]      any hard failure (the script raises)
# =============================================================================

import os
import sys

try:
    import unreal
except ImportError:  # pragma: no cover - only possible outside the editor
    sys.stderr.write("rename-kit-assets.py must run inside Unreal Editor's Python environment.\n")
    raise SystemExit(2)

EAL = unreal.EditorAssetLibrary

MAPGEO_DIR = "/Game/Trace/Art/Pack/MapGeometry"

# THE RENAME TABLE. rebake-arena-preserving.py carries the same four pairs in
# MESH_RENAMES and cites this file; if one of them ever changes, both change.
RENAMES = [
    ("octagon",   "SM_KitOctagon"),
    ("platform1", "SM_KitPlatform"),
    ("ramp1",     "SM_KitRamp"),
    ("ramp",      "SM_KitRampAlt"),
]

# The stray root duplicate. NOT deleted blind - see the note in phase_cleanup,
# which records what this asset actually is before removing it.
STRAY_ROOT_DUPLICATE = "/Game/ramp"

PHASE = os.environ.get("TRACE_RENAME_PHASE", "rename").strip().lower()


def log(message):
    unreal.log("[RENAME] {0}".format(message))


def fail(message):
    unreal.log_error("[RENAME][FAIL] {0}".format(message))
    raise RuntimeError(message)


def disk_path(package_path):
    """`/Game/X/Y` -> `<project>/Content/X/Y.uasset`."""
    if not package_path.startswith("/Game/"):
        return None
    project = unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_content_dir())
    return os.path.join(project, package_path[len("/Game/"):] + ".uasset")


def delete_and_sweep(package_path):
    """delete_asset, then make sure the FILE is gone too.

    MEASURED, on the first run of this script: EditorAssetLibrary.delete_asset
    returned True for all three redirectors and removed them from the asset
    registry, and all three 1.3 KB .uasset files were still on disk afterwards -
    so a `strings` sweep still found the old package paths, and a fresh clone
    would have re-registered three redirectors nothing points at. This is the
    same asset-registry-vs-filesystem gap bake-arena.sh documents for the .umap
    (bake-arena.sh:129-183). The registry delete is what makes the removal SAFE;
    this is what makes it TRUE.
    """
    if not EAL.delete_asset(package_path):
        return False
    path = disk_path(package_path)
    if path and os.path.isfile(path):
        try:
            os.remove(path)
            log("[DELETED] {0} - the registry delete left the file, removed it too".format(path))
        except OSError as exc:
            unreal.log_warning("[RENAME] could not remove {0}: {1}".format(path, exc))
    return True


def referencers(package_path):
    """Packages that reference @p package_path, excluding the package itself."""
    try:
        found = EAL.find_package_referencers_for_asset(package_path, load_assets_to_confirm=False)
    except Exception as exc:  # noqa: BLE001 - older signature
        unreal.log_warning("[RENAME] referencer query failed for {0}: {1}".format(package_path, exc))
        return None
    return sorted(str(name) for name in found if str(name) != package_path)


# -----------------------------------------------------------------------------
# PHASE 1 - rename
# -----------------------------------------------------------------------------

def phase_rename():
    log("[PLAN] rename {0} kit meshes under {1}".format(len(RENAMES), MAPGEO_DIR))
    for old, new in RENAMES:
        log("[PLAN]   {0:<10} -> {1}".format(old, new))

    moved = 0
    skipped = 0
    for old, new in RENAMES:
        old_path = "{0}/{1}".format(MAPGEO_DIR, old)
        new_path = "{0}/{1}".format(MAPGEO_DIR, new)

        if EAL.does_asset_exist(new_path):
            # Idempotent re-run, or a half-finished earlier run. Either way the
            # destination is the truth; do not rename over it.
            log("[MOVED] {0} already exists - skipping {1}".format(new_path, old_path))
            skipped += 1
            continue

        if not EAL.does_asset_exist(old_path):
            fail("neither {0} nor {1} exists".format(old_path, new_path))

        before = referencers(old_path)
        log("[REFS] {0}: {1} referencer(s) {2}".format(
            old_path, -1 if before is None else len(before), before if before else ""))

        if not EAL.rename_asset(old_path, new_path):
            fail("rename_asset refused {0} -> {1}".format(old_path, new_path))

        # Read the destination back off the registry rather than trusting the
        # return value: a rename that reports success and leaves nothing at the
        # new path is the failure mode worth one line of code.
        if not EAL.does_asset_exist(new_path):
            fail("rename reported success but {0} does not exist".format(new_path))

        asset = EAL.load_asset(new_path)
        log("[MOVED] {0} -> {1}  class={2}".format(
            old_path, new_path, asset.get_class().get_name() if asset else "?"))
        moved += 1

    # Best effort at the engine's own fixup, purely so the log says which route
    # ran. IAssetTools::FixupReferencers has no UFUNCTION in 5.8, so this is
    # expected to report "not exposed" and the regeneration route (the re-bake)
    # is what does the work. See the header.
    try:
        tools = unreal.AssetToolsHelpers.get_asset_tools()
        fixup = getattr(tools, "fixup_referencers", None)
        if fixup is None:
            log("[PLAN] AssetTools.fixup_referencers is not exposed to Python in this engine; "
                "the re-bake regenerates every referencing package instead (see the header)")
        else:
            redirectors = []
            for old, _new in RENAMES:
                obj = EAL.load_asset("{0}/{1}".format(MAPGEO_DIR, old))
                if obj is not None and obj.get_class().get_name() == "ObjectRedirector":
                    redirectors.append(obj)
            if redirectors:
                fixup(redirectors)
                log("[PLAN] AssetTools.fixup_referencers ran over {0} redirector(s)".format(len(redirectors)))
    except Exception as exc:  # noqa: BLE001 - a probe, never a hard failure
        unreal.log_warning("[RENAME] fixup probe: {0}".format(exc))

    EAL.save_directory(MAPGEO_DIR, only_if_is_dirty=False, recursive=True)
    log("[DONE] renamed {0}, skipped {1} (already named), of {2}".format(moved, skipped, len(RENAMES)))
    census()


# -----------------------------------------------------------------------------
# PHASE 2 - cleanup (run AFTER the re-bake)
# -----------------------------------------------------------------------------

def phase_cleanup():
    log("[PLAN] delete the four redirectors and the stray root duplicate, "
        "each only if the asset registry says nothing references it")

    deleted = 0
    kept = 0
    for old, _new in RENAMES:
        old_path = "{0}/{1}".format(MAPGEO_DIR, old)
        if not EAL.does_asset_exist(old_path):
            log("[DELETED] {0} is already gone".format(old_path))
            continue

        obj = EAL.load_asset(old_path)
        klass = obj.get_class().get_name() if obj is not None else "?"
        refs = referencers(old_path)
        log("[REFS] {0} ({1}): {2} referencer(s) {3}".format(
            old_path, klass, -1 if refs is None else len(refs), refs if refs else ""))

        if refs is None or refs:
            log("[KEPT] {0} still has referencers - NOT deleted. Something outside the re-baked "
                "level names it, and that has to be looked at before this is safe.".format(old_path))
            kept += 1
            continue

        if not delete_and_sweep(old_path):
            fail("delete_asset refused {0}".format(old_path))
        log("[DELETED] {0}".format(old_path))
        deleted += 1

    # --- the stray root duplicate ------------------------------------------------------------
    #
    # MAP §10.4 calls this "the same FBX imported to the content root". THAT IS
    # NOT QUITE WHAT IT IS, and the difference is recorded here rather than
    # rounded off: all three ramp assets name the same SOURCE PATH
    # (~/OneDrive/Documents/ramp.fbx) but carry three different import FileMD5s
    # and three different timestamps - MapGeometry/ramp 1787255060,
    # /Game/ramp 1787258909, MapGeometry/ramp1 1787263328. The collaborator
    # re-exported the same filename three times; this is the MIDDLE revision,
    # and it is the one nothing in the project uses.
    #
    # It is still removed, because the reason §10.4 gives that DOES hold is the
    # load-bearing one: nothing references it. It is committed to git
    # (78b6a26 "updated middle of map"), so `git checkout -- Content/ramp.uasset`
    # brings it back, and the tranche's backup directory holds a copy as well.
    if EAL.does_asset_exist(STRAY_ROOT_DUPLICATE):
        refs = referencers(STRAY_ROOT_DUPLICATE)
        log("[REFS] {0}: {1} referencer(s) {2}".format(
            STRAY_ROOT_DUPLICATE, -1 if refs is None else len(refs), refs if refs else ""))
        if refs is None or refs:
            log("[KEPT] {0} has referencers - NOT deleted".format(STRAY_ROOT_DUPLICATE))
            kept += 1
        elif delete_and_sweep(STRAY_ROOT_DUPLICATE):
            log("[DELETED] {0} (the stray root import; see the note in this script)".format(
                STRAY_ROOT_DUPLICATE))
            deleted += 1
        else:
            fail("delete_asset refused {0}".format(STRAY_ROOT_DUPLICATE))
    else:
        log("[DELETED] {0} is already gone".format(STRAY_ROOT_DUPLICATE))

    log("[DONE] deleted {0}, kept {1}".format(deleted, kept))
    census()


# -----------------------------------------------------------------------------
# Census - what the folder holds now, whichever phase ran
# -----------------------------------------------------------------------------

def census():
    paths = sorted(EAL.list_assets(MAPGEO_DIR, recursive=True, include_folder=False))
    log("[DONE] {0} holds {1} asset(s):".format(MAPGEO_DIR, len(paths)))
    for path in paths:
        obj = EAL.load_asset(path)
        log("[DONE]   {0:<58} {1}".format(path, obj.get_class().get_name() if obj else "?"))
    log("[DONE] {0} exists: {1}".format(
        STRAY_ROOT_DUPLICATE, EAL.does_asset_exist(STRAY_ROOT_DUPLICATE)))


if PHASE == "rename":
    phase_rename()
elif PHASE == "cleanup":
    phase_cleanup()
elif PHASE == "report":
    census()
else:
    fail("TRACE_RENAME_PHASE must be rename, cleanup or report (got '{0}')".format(PHASE))

log("PHASE {0} OK".format(PHASE))
