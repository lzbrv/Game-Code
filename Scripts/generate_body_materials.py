# =============================================================================
# Trace — generate_body_materials.py
#
# Runs INSIDE the editor (UnrealEditor-Cmd -run=pythonscript, REAL RHI — never
# -nullrhi: three master materials are compiled here and a null RHI leaves them
# uncompiled on disk, the same trap documented in Scripts/import-rocco.sh and
# Scripts/bake-arena.sh). Driven by Scripts/import-characters.sh --stage
# materials — do not invoke this by hand.
#
# Authors the body-material set for the ten generated characters
# (PIPELINE_DESIGN.md §4.2-4.3, integrator conflict #2 resolution):
#
#   /Game/Trace/Characters/Shared/Materials/
#       M_TraceBodySuit            parent: lit opaque suit shell
#       M_TraceBodyGlow            parent: team-glow panels
#       M_TraceBodyAccent          parent: per-character accent trim
#       MI_Body_SuitHead           shared MI  (head shell, the x1.4 lift)
#       MI_Body_Inset              shared MI  (panel-line near-black)
#       MI_Body_Glow               shared MI  (parent defaults)
#       MI_Body_<Name>_Suit   x10  per-character roughness (sheet §0.5 identity
#                                  channel — the integrator's conflict-2 ruling:
#                                  ten suit MIs instead of one shared MI_Body_Suit)
#       MI_Body_<Name>_Accent x10  per-character accent hue
#
#   3 parents + 3 shared MIs + 10 suit MIs + 10 accent MIs = 26 assets.
#
# THE PARAMETER NAMES ARE A RUNTIME CONTRACT, NOT A STYLE CHOICE.
# ATraceCharacter::ApplyColorToSkeletalMesh (TraceCharacter.cpp:3810-3861)
# builds one MID per slot and on EVERY color refresh stomps, on ALL slots:
#
#   vectors "Paint Tint" / "Color" / "BaseColor" / "Tint" / "EmissiveColor"
#       <- BodyColor (team color, carrier-blended toward white, dead x0.2)
#   scalar  "EmissivePower"  <- 8 normal / 30 carrier / 0 dead
#
# and Elle's cloak (TraceAbilitySetElle.cpp:100-112) additionally zeroes
# scalars "EmissivePower"/"EmissiveStrength"/"Glow" and writes near-black into
# the same vector list. Setting a parameter a material does not have is a
# silent no-op, so the design rule is:
#
#   A PARAMETER CARRIES PER-TEAM/STATE MEANING IFF IT USES ONE OF THE STOMPED
#   NAMES; EVERYTHING CHARACTER-FIXED USES NAMES OUTSIDE THAT LIST.
#
#   M_TraceBodySuit   exposes  SuitColor x Tint -> BaseColor, Roughness.
#                     "Tint" is stomped -> in-game albedo becomes
#                     0.10 x TraceTeamColor (the bible's Dim(TeamColor, 0.10))
#                     and carrier/dead/cloak flow through for free. SuitColor,
#                     Roughness are character-fixed and never touched.
#   M_TraceBodyGlow   exposes  EmissiveColor x EmissivePower x 0.2125.
#                     Both names are stomped BY DESIGN: team glow 8 x 0.2125
#                     = 1.7 (bible §4.4 Glow tier), carrier 30 x 0.2125 =
#                     6.375, dead 0.
#   M_TraceBodyAccent exposes  AccentColor (x0.02 -> BaseColor; x AccentGlow x
#                     0.2125 -> Emissive). NEITHER name is in the stomp list,
#                     so the accent hue can never be team-stomped (bible §2.3).
#                     W3-CHARWIRE adds the two 1-line C++ deltas of PIPELINE §4.4
#                     (AccentGlow joins the state scalar + the cloak's zero
#                     list); the HUE stays character-fixed either way.
#
# The forbidden/required parameter-name sets are ASSERTED below by reading the
# saved assets back — a rename here would ship a body that silently ignores
# team color, and nothing else would catch it before a match.
#
# used_with_skeletal_mesh is REQUIRED on all three parents and its absence is
# silent in the editor and fatal in -game: without MATUSAGE_SkeletalMesh the
# skinned vertex-factory permutation is never compiled and every body renders
# as the grey default checkerboard. The editor's auto-repair for the flag is
# gated on not running as a game, and every run of this project is -game. Same
# trap as M_TraceNeon (Scripts/generate_content.py) and M_RoccoPlaceholder
# (Scripts/import_rocco.py:530-537).
#
# SINGLE SOURCE FOR THE PER-CHARACTER VALUES: Scripts/character_bodies.py
# (CHARACTERS — accent linears re-verified against TraceCharacterRoster.cpp,
# roughness per CHARACTER_SHEETS §0.5). This script imports that table rather
# than repeating the literals (the B5 lesson).
#
# IDEMPOTENT WITHOUT DELETING: existing parents are rebuilt in place
# (delete_all_material_expressions + collect_garbage + a fresh graph), existing
# MIs get their parent and parameters rewritten in place. Nothing calls
# delete_asset — an in-session delete leaves an Asset Registry ghost for the
# rest of the session (the import-rocco lesson), and keeping the packages keeps
# their identity: same asset paths, same package GUIDs, no redirectors, nothing
# downstream to re-point. A GUID-fresh rebuild is the wrapper's job:
# import-characters.sh --stage materials --force wipes the .uasset files BEFORE
# the editor starts.
#
# WHAT A RE-RUN DOES COST (◆MEASURED over seven runs, 2026-08-24): all 26
# .uasset FILES are rewritten and their bytes differ run to run — the three
# parents recompile (material StateId/shader map) and the 23 instances cache
# the parent's new state. Sizes stay in a narrow band (Suit 8.1 KB, Glow ~9.4
# KB, Accent ~10.2 KB); the parameter VALUES are identical, which is what the
# readback asserts. So: re-run freely for verification, but expect 26 dirty
# binaries in `git status` afterwards, and do not re-run casually right before
# someone reviews the diff.
#
# VERDICT: grep the log for "[generate-body-materials] EXIT=0". The commandlet
# process exit code is the engine's error count and is nonzero on healthy runs
# (see generate-data-assets.py:38-43).
# =============================================================================

import os
import sys

import unreal

# The per-character table lives beside this script; the editor's sys.path does
# not include Scripts/ when running via -run=pythonscript.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from character_bodies import CHARACTERS, CHARACTER_ORDER  # noqa: E402

MEL = unreal.MaterialEditingLibrary
EAL = unreal.EditorAssetLibrary

MATERIAL_DIR = "/Game/Trace/Characters/Shared/Materials"

PARENT_SUIT = "M_TraceBodySuit"
PARENT_GLOW = "M_TraceBodyGlow"
PARENT_ACCENT = "M_TraceBodyAccent"
PARENT_NAMES = (PARENT_SUIT, PARENT_GLOW, PARENT_ACCENT)

# The bible/PIPELINE §4.2 constants.
SUIT_DEFAULT = (0.10, 0.10, 0.10, 1.0)     # SuitColor parent default
SUIT_HEAD = (0.14, 0.14, 0.14, 1.0)        # x1.4 head lift (bible §4.4)
SUIT_INSET = (0.006, 0.007, 0.010, 1.0)    # ~#111319 panel-line black
GLOW_BASE = (0.005, 0.005, 0.008)          # Glow parent's near-black base
NEON_NEUTRAL = (0.18, 0.78, 1.0, 1.0)      # EmissiveColor default (bible §7.5)
GLOW_SCALE = 0.2125                        # 8 x 0.2125 = 1.7, the Glow tier
ACCENT_BASE_SCALE = 0.02                   # AccentColor x 0.02 -> BaseColor
ACCENT_DEFAULT = (1.0, 1.0, 1.0, 1.0)      # parent-only default; every body
                                           # wears a per-character MI, so white
                                           # glow = "the MI binding is missing"
ROUGHNESS_DEFAULT = 0.45

SKELETAL_USAGE = unreal.MaterialUsage.MATUSAGE_SKELETAL_MESH

# The stomp list, verbatim from ApplyColorToSkeletalMesh + Elle's cloak.
STOMPED_VECTORS = ("Paint Tint", "Color", "BaseColor", "Tint", "EmissiveColor")
STOMPED_SCALARS = ("EmissivePower", "EmissiveStrength", "Glow")

EPS = 1e-4

_failures = []


def log(msg):
    unreal.log("[Trace] {0}".format(msg))


def fail(msg):
    _failures.append(msg)
    unreal.log_error("[Trace] {0}".format(msg))


# -----------------------------------------------------------------------------
# Expression helpers — the generate_content.py:196-330 pattern, trimmed to the
# nodes these three graphs need. Every connection is checked and reported
# loudly; a silent miss here is a black material, not an error.
# -----------------------------------------------------------------------------

def new_expression(material, klass, x, y):
    return MEL.create_material_expression(material, klass, x, y)


def link(from_expr, from_pin, to_expr, to_pin):
    if not MEL.connect_material_expressions(from_expr, from_pin, to_expr, to_pin):
        raise RuntimeError("could not connect '{0}' -> '{1}'".format(from_pin or "out", to_pin))


def link_property(expr, pin, prop):
    if not MEL.connect_material_property(expr, pin, prop):
        raise RuntimeError("could not connect '{0}' to material property {1}".format(pin or "out", prop))


def vector_param(material, name, value, x, y, priority=0):
    expr = new_expression(material, unreal.MaterialExpressionVectorParameter, x, y)
    expr.set_editor_property("parameter_name", name)
    expr.set_editor_property("default_value", unreal.LinearColor(*value))
    expr.set_editor_property("group", "Trace|Body")
    expr.set_editor_property("sort_priority", priority)
    return expr


def scalar_param(material, name, value, x, y, priority=0):
    expr = new_expression(material, unreal.MaterialExpressionScalarParameter, x, y)
    expr.set_editor_property("parameter_name", name)
    expr.set_editor_property("default_value", float(value))
    expr.set_editor_property("group", "Trace|Body")
    expr.set_editor_property("sort_priority", priority)
    return expr


def constant(material, value, x, y):
    expr = new_expression(material, unreal.MaterialExpressionConstant, x, y)
    expr.set_editor_property("r", float(value))
    return expr


def constant3(material, rgb, x, y):
    expr = new_expression(material, unreal.MaterialExpressionConstant3Vector, x, y)
    expr.set_editor_property("constant", unreal.LinearColor(rgb[0], rgb[1], rgb[2], 1.0))
    return expr


def multiply(material, a, b, x, y):
    expr = new_expression(material, unreal.MaterialExpressionMultiply, x, y)
    link(a, "", expr, "A")
    link(b, "", expr, "B")
    return expr


# -----------------------------------------------------------------------------
# Parents
# -----------------------------------------------------------------------------

def ensure_parent(name):
    """Load-or-create the parent material, with a clean expression slate.

    Existing assets are REBUILT IN PLACE rather than deleted: delete_asset
    leaves an Asset Registry ghost for the rest of the session (import_rocco.py
    sweep() documents the failure), and keeping the asset keeps its GUID so
    re-runs do not churn LFS binaries. A from-scratch rebuild goes through the
    shell wrapper's --force pre-launch wipe instead.
    """
    path = "{0}/{1}".format(MATERIAL_DIR, name)
    if EAL.does_asset_exist(path):
        material = EAL.load_asset(path)
        if not isinstance(material, unreal.Material):
            fail("{0} exists but is a {1}, not a Material; wipe the folder with "
                 "import-characters.sh --stage materials --force".format(path, type(material).__name__))
            return None
        MEL.delete_all_material_expressions(material)
        # GC between the teardown and the rebuild. Without it the just-deleted
        # expression objects are still outered to this material when it is
        # saved, and each in-place rebuild grew the parent .uasset (MEASURED:
        # M_TraceBodyGlow 7885 -> 9047 -> 9710 bytes over three runs) — a slow
        # LFS-churn leak on an otherwise idempotent script.
        unreal.SystemLibrary.collect_garbage()
        log("Rebuilding {0} in place.".format(path))
        return material
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    material = tools.create_asset(name, MATERIAL_DIR, unreal.Material, unreal.MaterialFactoryNew())
    if material is None:
        fail("AssetTools refused to create {0}".format(path))
    return material


def common_parent_properties(material):
    material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_DEFAULT_LIT)
    material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_OPAQUE)
    material.set_editor_property("two_sided", False)
    # REQUIRED — silent in the editor, fatal in -game (see the header).
    material.set_editor_property("used_with_skeletal_mesh", True)


def finish_material(material):
    MEL.recompile_material(material)
    if not EAL.save_loaded_asset(material, only_if_is_dirty=False):
        fail("could not save {0}".format(material.get_path_name()))
    else:
        log("Saved {0}".format(material.get_path_name().split(".")[0]))


def build_suit_parent():
    material = ensure_parent(PARENT_SUIT)
    if material is None:
        return
    common_parent_properties(material)
    # BaseColor = SuitColor (character-fixed) x Tint (the stomp target).
    # In-game: 0.10 x TraceTeamColor = Dim(TeamColor, 0.10), bible §4.4.
    # Portrait/no-runtime: Tint stays white -> neutral 0.10 grey (bible §7.5).
    suit = vector_param(material, "SuitColor", SUIT_DEFAULT, -600, -100, priority=0)
    tint = vector_param(material, "Tint", (1.0, 1.0, 1.0, 1.0), -600, 150, priority=1)
    albedo = multiply(material, suit, tint, -300, 0)
    link_property(albedo, "", unreal.MaterialProperty.MP_BASE_COLOR)
    # Per-character sheet roughness rides the MI (0.25-0.60); 0.45 is the
    # PIPELINE §4.2 parent default. Metallic is left unconnected = 0.
    rough = scalar_param(material, "Roughness", ROUGHNESS_DEFAULT, -600, 400, priority=2)
    link_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS)
    finish_material(material)


def build_glow_parent():
    material = ensure_parent(PARENT_GLOW)
    if material is None:
        return
    common_parent_properties(material)
    # Near-black base so the lit surface reads as dark panel around the glow.
    base = constant3(material, GLOW_BASE, -600, -100)
    link_property(base, "", unreal.MaterialProperty.MP_BASE_COLOR)
    # Emissive = EmissiveColor x EmissivePower x 0.2125. Both parameter names
    # are stomp targets BY DESIGN (team hue / 8-30-0 state scalar); the 0.2125
    # constant (GlowScale) maps EmissivePower 8 onto the bible's Glow 1.7 tier.
    color = vector_param(material, "EmissiveColor", NEON_NEUTRAL, -600, 150, priority=0)
    power = scalar_param(material, "EmissivePower", 8.0, -600, 400, priority=1)
    scale = constant(material, GLOW_SCALE, -600, 550)
    emissive = multiply(material, multiply(material, color, power, -300, 250),
                        scale, -100, 300)
    link_property(emissive, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    finish_material(material)


def build_accent_parent():
    material = ensure_parent(PARENT_ACCENT)
    if material is None:
        return
    common_parent_properties(material)
    # AccentColor / AccentGlow are OUTSIDE the stomp list on purpose: the hue
    # is per-character identity and must survive every team/state repaint.
    # (W3-CHARWIRE wires AccentGlow into the state scalar + cloak per §4.4.)
    accent = vector_param(material, "AccentColor", ACCENT_DEFAULT, -600, -100, priority=0)
    base = multiply(material, accent, constant(material, ACCENT_BASE_SCALE, -600, 120), -300, 0)
    link_property(base, "", unreal.MaterialProperty.MP_BASE_COLOR)
    glow = scalar_param(material, "AccentGlow", 8.0, -600, 400, priority=1)
    scale = constant(material, GLOW_SCALE, -600, 550)
    emissive = multiply(material, multiply(material, accent, glow, -300, 250),
                        scale, -100, 300)
    link_property(emissive, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    finish_material(material)


# -----------------------------------------------------------------------------
# Parent verification — read back off the SAVED assets, never off the objects
# this script just mutated (the generate_content.py:600-640 round-trip rule).
# -----------------------------------------------------------------------------

# name -> (expected vector params {name: default}, expected scalar params,
#          forbidden vector names, forbidden scalar names)
PARENT_CONTRACT = {
    PARENT_SUIT: (
        {"SuitColor": SUIT_DEFAULT, "Tint": (1.0, 1.0, 1.0, 1.0)},
        {"Roughness": ROUGHNESS_DEFAULT},
        ("Paint Tint", "Color", "BaseColor", "EmissiveColor"),
        STOMPED_SCALARS,
    ),
    PARENT_GLOW: (
        {"EmissiveColor": NEON_NEUTRAL},
        {"EmissivePower": 8.0},
        ("Paint Tint", "Color", "BaseColor", "Tint"),
        ("EmissiveStrength", "Glow"),
    ),
    PARENT_ACCENT: (
        {"AccentColor": ACCENT_DEFAULT},
        {"AccentGlow": 8.0},
        STOMPED_VECTORS,
        STOMPED_SCALARS,
    ),
}


def close(a, b):
    return abs(float(a) - float(b)) <= EPS


def vclose(color, expected):
    return (close(color.r, expected[0]) and close(color.g, expected[1])
            and close(color.b, expected[2]) and close(color.a, expected[3]))


def verify_parent(name):
    path = "{0}/{1}".format(MATERIAL_DIR, name)
    material = EAL.load_asset(path)
    if material is None:
        fail("{0} did not reach disk".format(path))
        return None

    # The flag every body render depends on, checked twice: the serialized
    # property and the engine's own usage query.
    if not material.get_editor_property("used_with_skeletal_mesh"):
        fail("{0}: used_with_skeletal_mesh is FALSE".format(name))
    if not MEL.has_material_usage(material, SKELETAL_USAGE):
        fail("{0}: has_material_usage(SkeletalMesh) is FALSE".format(name))

    expected_vec, expected_scalar, forbidden_vec, forbidden_scalar = PARENT_CONTRACT[name]

    vec_names = set(str(n) for n in MEL.get_vector_parameter_names(material))
    scalar_names = set(str(n) for n in MEL.get_scalar_parameter_names(material))

    missing = sorted(set(expected_vec) - vec_names) + sorted(set(expected_scalar) - scalar_names)
    if missing:
        fail("{0}: missing parameter(s) {1}".format(name, ", ".join(missing)))
    # The stomp-law assert: a forbidden name here means the runtime would
    # repaint something character-fixed (or double-drive a glow).
    bad = sorted(vec_names.intersection(forbidden_vec)) \
        + sorted(scalar_names.intersection(forbidden_scalar))
    if bad:
        fail("{0}: carries STOMPED parameter name(s) {1} — the team/state repaint "
             "would land on a character-fixed channel (see the header contract)"
             .format(name, ", ".join(bad)))
    extra = sorted((vec_names - set(expected_vec)) | (scalar_names - set(expected_scalar)))
    if extra:
        fail("{0}: unexpected parameter(s) {1} — the contract is closed; extend "
             "PARENT_CONTRACT deliberately or remove them".format(name, ", ".join(extra)))

    for pname, want in sorted(expected_vec.items()):
        got = MEL.get_material_default_vector_parameter_value(material, pname)
        if not vclose(got, want):
            fail("{0}.{1}: default ({2:.4f},{3:.4f},{4:.4f},{5:.4f}) != expected {6}"
                 .format(name, pname, got.r, got.g, got.b, got.a, want))
    for pname, want in sorted(expected_scalar.items()):
        got = MEL.get_material_default_scalar_parameter_value(material, pname)
        if not close(got, want):
            fail("{0}.{1}: default {2:.4f} != expected {3}".format(name, pname, got, want))

    # The flag is printed as a VALUE, not implied by the absence of a failure:
    # this line is the evidence a later wave greps when a body renders grey.
    # Defaults are printed too, so the log alone describes the contract.
    defaults = []
    for pname in sorted(vec_names):
        v = MEL.get_material_default_vector_parameter_value(material, pname)
        defaults.append("{0}=({1:.3f},{2:.3f},{3:.3f})".format(pname, v.r, v.g, v.b))
    for pname in sorted(scalar_names):
        defaults.append("{0}={1:.3f}".format(
            pname, MEL.get_material_default_scalar_parameter_value(material, pname)))
    log("{0}: used_with_skeletal_mesh={1}/has_material_usage={2}, params verified "
        "[{3}]".format(
            name,
            bool(material.get_editor_property("used_with_skeletal_mesh")),
            bool(MEL.has_material_usage(material, SKELETAL_USAGE)),
            " ".join(defaults) or "no parameters"))
    return material


# -----------------------------------------------------------------------------
# Instances
# -----------------------------------------------------------------------------

def mi_specs():
    """The 23 instances: (asset name, parent name, vector overrides, scalar overrides)."""
    specs = [
        ("MI_Body_SuitHead", PARENT_SUIT, {"SuitColor": SUIT_HEAD}, {}),
        ("MI_Body_Inset", PARENT_SUIT, {"SuitColor": SUIT_INSET}, {}),
        ("MI_Body_Glow", PARENT_GLOW, {}, {}),
    ]
    for cname in CHARACTER_ORDER:
        row = CHARACTERS[cname]
        specs.append(("MI_Body_{0}_Suit".format(cname), PARENT_SUIT,
                      {"SuitColor": SUIT_DEFAULT},
                      {"Roughness": row["roughness"]}))
        a = row["accent"]
        specs.append(("MI_Body_{0}_Accent".format(cname), PARENT_ACCENT,
                      {"AccentColor": (a[0], a[1], a[2], 1.0)}, {}))
    return specs


def build_instance(name, parent, vectors, scalars):
    """Create-if-missing, then rewrite parent + parameters in place every run
    (the import-rocco.sh MI-churn rule: parameters land without --force)."""
    path = "{0}/{1}".format(MATERIAL_DIR, name)
    inst = None
    if EAL.does_asset_exist(path):
        inst = EAL.load_asset(path)
        if not isinstance(inst, unreal.MaterialInstanceConstant):
            fail("{0} exists but is a {1}, not a MaterialInstanceConstant"
                 .format(path, type(inst).__name__))
            return
    if inst is None:
        tools = unreal.AssetToolsHelpers.get_asset_tools()
        inst = tools.create_asset(name, MATERIAL_DIR, unreal.MaterialInstanceConstant,
                                  unreal.MaterialInstanceConstantFactoryNew())
        if inst is None:
            fail("AssetTools refused to create {0}".format(path))
            return
    MEL.set_material_instance_parent(inst, parent)
    for pname, value in sorted(vectors.items()):
        MEL.set_material_instance_vector_parameter_value(
            inst, pname, unreal.LinearColor(*value))
    for pname, value in sorted(scalars.items()):
        MEL.set_material_instance_scalar_parameter_value(inst, pname, float(value))
    if not EAL.save_loaded_asset(inst, only_if_is_dirty=False):
        fail("could not save {0}".format(path))


def verify_instance(name, parent_name, vectors, scalars):
    path = "{0}/{1}".format(MATERIAL_DIR, name)
    inst = EAL.load_asset(path)
    if inst is None:
        fail("{0} did not reach disk".format(path))
        return
    parent = inst.get_editor_property("parent")
    if parent is None or parent.get_name() != parent_name:
        fail("{0}: parent is {1}, expected {2}".format(
            name, parent.get_name() if parent else "None", parent_name))
    # The usage flag as the MESH SLOT will actually ask for it — off the
    # instance, derived from the parent (the bind_pack_materials.py rule).
    if not MEL.has_material_usage(inst, SKELETAL_USAGE):
        fail("{0}: instance reports no SkeletalMesh usage".format(name))
    readback = []
    for pname, want in sorted(vectors.items()):
        got = MEL.get_material_instance_vector_parameter_value(inst, pname)
        if not vclose(got, want):
            fail("{0}.{1}: ({2:.4f},{3:.4f},{4:.4f},{5:.4f}) != expected {6}"
                 .format(name, pname, got.r, got.g, got.b, got.a, want))
        readback.append("{0}=({1:.3f},{2:.3f},{3:.3f})".format(pname, got.r, got.g, got.b))
    for pname, want in sorted(scalars.items()):
        got = MEL.get_material_instance_scalar_parameter_value(inst, pname)
        if not close(got, want):
            fail("{0}.{1}: {2:.4f} != expected {3}".format(name, pname, got, want))
        readback.append("{0}={1:.3f}".format(pname, got))
    # One readback line per instance: the log alone answers "what colour/
    # roughness does <Name> actually carry" without opening the editor.
    log("  {0}: parent={1} skel_usage={2} {3}".format(
        name, parent_name, bool(MEL.has_material_usage(inst, SKELETAL_USAGE)),
        " ".join(readback) or "(parent defaults, no overrides)"))


# -----------------------------------------------------------------------------
# Main
# -----------------------------------------------------------------------------

def main():
    if len(CHARACTER_ORDER) != 10:
        fail("character_bodies.CHARACTER_ORDER has {0} names, expected 10"
             .format(len(CHARACTER_ORDER)))

    build_suit_parent()
    build_glow_parent()
    build_accent_parent()

    parents = {}
    for name in PARENT_NAMES:
        parents[name] = verify_parent(name)
    if any(parents[n] is None for n in PARENT_NAMES):
        report()
        return

    specs = mi_specs()
    for name, parent_name, vectors, scalars in specs:
        build_instance(name, parents[parent_name], vectors, scalars)
    for name, parent_name, vectors, scalars in specs:
        verify_instance(name, parent_name, vectors, scalars)

    # Disk census: exactly the 26 owned assets, nothing else. A stray here
    # means a rename left a redirector or someone else is writing this folder.
    expected = set(PARENT_NAMES) | set(s[0] for s in specs)
    on_disk = set(p.split("/")[-1].split(".")[0]
                  for p in EAL.list_assets(MATERIAL_DIR, recursive=True, include_folder=False))
    missing = sorted(expected - on_disk)
    strays = sorted(on_disk - expected)
    if missing:
        fail("missing from {0}: {1}".format(MATERIAL_DIR, ", ".join(missing)))
    if strays:
        fail("stray asset(s) in {0}: {1}".format(MATERIAL_DIR, ", ".join(strays)))
    log("{0}/{1} material assets on disk in {2} (3 parents + {3} instances)".format(
        len(expected - set(missing)), 26, MATERIAL_DIR, len(specs)))

    report()


def report():
    log("")
    log("================ BODY MATERIALS REPORT ================")
    if _failures:
        log("{0} PROBLEM(S):".format(len(_failures)))
        for f in _failures:
            log("  - {0}".format(f))
    else:
        log("no problems reported.")
    log("================ END REPORT ================")
    # The grep-verdict convention (generate-data-assets.py:38-43): the process
    # exit code is the engine's error count, not this script's verdict.
    log("[generate-body-materials] EXIT={0}".format(1 if _failures else 0))


main()
