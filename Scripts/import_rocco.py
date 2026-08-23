# =============================================================================
# Trace — import_rocco.py
#
# Runs INSIDE the editor (UnrealEditor-Cmd -run=pythonscript). Imports the test
# character model for Rocco from Art/Characters/Rocco/RoccoTest.fbx through
# Interchange and reports what it actually measured, in Unreal units, next to
# the Mannequin the rest of the game is built around.
#
# Driven by Scripts/import-rocco.sh — do not invoke this by hand.
#
#   Art/Characters/Rocco/RoccoTest.fbx
#        |
#        v
#   /Game/Trace/Characters/Rocco/SK_Rocco             SkeletalMesh
#   /Game/Trace/Characters/Rocco/SK_Rocco_Skeleton    Skeleton, 25 bones: the
#                                                     armature root plus 24 rig bones
#   /Game/Trace/Characters/Rocco/SK_Rocco_PhysicsAsset PhysicsAsset
#   /Game/Trace/Characters/Rocco/M_RoccoPlaceholder   Material
#   /Game/Trace/Characters/Rocco/MI_Rocco_*           one instance of it per
#                                                     material slot, ten of them
#
# THREE THINGS ABOUT THIS FBX THAT DECIDE HOW IT IS IMPORTED
#
#  1. IT CONTAINS THE CHARACTER TWICE. Two skinned meshes, two 24-bone
#     armatures, identical topology (4172 polygons each):
#
#       Model "Rocco Model 1"  <- Geometry char1.001, skinned by "target_character"
#            2515 verts, UV set "UVMap", a greyscale vertex-colour layer, and
#            TEN material slots.  <-- this is the finished model
#       Model "char1"          <- Geometry char1,     skinned by "Armature"
#            2220 verts, UV set "uv", NO material layer at all.
#            <-- the bare base mesh the artist retargeted from
#
#     The two are rotated 90 degrees from each other about the up axis, so they
#     are not interchangeable. Nothing in the file marks one as "the" model, so
#     the rule this script applies is the one fact that separates them: KEEP THE
#     SKELETAL MESH THAT CARRIES THE TEN MATERIAL SLOTS. The twin has no material
#     layer at all, so the engine hands it ONE unnamed default slot — which is
#     why the test is "many slots versus one" and not "some versus none". That is
#     asserted at import time (see pick_rocco) rather than assumed, and the twin
#     is deleted.
#
#  2. THE SKELETON IS NOT THE MANNEQUIN'S. Bones are Hips/Spine/Spine01/Spine02/
#     LeftUpLeg/.../LeftHand/RightHand/neck/Head — a generic, Mixamo-shaped rig.
#     Not one UE Mannequin bone name (pelvis, spine_01, thigh_l, hand_r, root)
#     appears. ABP_Unarmed cannot drive this mesh and the third-person knife's
#     "hand_r" socket does not exist on it. That is not this script's problem to
#     solve — it imports the art and MEASURES the gap. The report at the end
#     prints every bone with its component-space position so the next step does
#     not have to guess, and THE NEXT STEP IS Scripts/retarget-rocco.sh, which
#     reads exactly those names to build IK_Rocco's chains and bakes ABP_Unarmed
#     onto this skeleton. Run it after this one or Rocco does not move.
#
#  3. THE COLOUR IS NOT IN THE FILE, AND SAYING SO IS NOT ENOUGH. Six FBX Video
#     nodes reference C:\Users\ranen\OneDrive\Documents\Blender\textures\packed\<name>
#     — external, extension-less, on a machine that is not this one — and there
#     is not one embedded Content block anywhere in the FBX. All ten FBX
#     materials carry the same 0.8 grey DiffuseColor, which is Blender's
#     untouched default and means only that the albedo was coming from those
#     missing textures. So there is no colour to import: material and texture
#     import are switched OFF (they would produce ten identical grey materials
#     and six failed fetches) and ROCCO ARRIVES WITH NO ALBEDO ANYWHERE ON HIM.
#     What other players see is the right silhouette, the right skinning, the
#     right motion, and one flat colour over all of it.
#
#     TWO THINGS FOLLOW, AND BOTH ARE THIS SCRIPT'S JOB.
#
#     First, that verdict is MEASURED rather than asserted. read_fbx_surfaces
#     opens the file and counts the embedded images and lists the external
#     references, and the report prints what it found — so the paragraph you are
#     reading cannot quietly go stale, and a re-export that finally carries its
#     images flips the report without anybody editing it. The previous version of
#     this comment was the only place the fact lived, and a comment cannot tell
#     you the day it stopped being true.
#
#     Second, colour is not the ONLY per-slot data in the file, and the rest of
#     it was being thrown away. Shininess and ReflectionFactor DIFFER slot to
#     slot: roughness spans 0.52 to 1.00 and two of the ten slots are metal.
#     Pointing all ten at one material made Rocco flatter than the file says he
#     is. Each slot now wears its own MI_Rocco_* instance of the placeholder
#     carrying those two numbers. Nothing is invented — there is no per-slot
#     colour to invent from — he is simply as un-flat as the source allows.
#
#     THE FIX FOR THE COLOUR IS A RE-EXPORT: Path Mode = Copy with "Embed
#     Textures" ticked, or the six PNGs by hand. The slot names are preserved, so
#     a textured re-import lands on exactly these slots.
#
# AND ONE CONSEQUENCE OF (1) YOU WILL SEE IN EVERY BONE NAME
#   The two armatures use the SAME 24 bone names. The FBX translator imports both,
#   hits the collision, and uniquifies whichever it reaches second — which is
#   Rocco's — so the kept mesh lands wearing Hips1, LeftHand1, Spine011,
#   head_end1, under a root bone called "target_character" (the armature's Null).
#
#   THOSE NAMES CANNOT BE FIXED FROM A COMMANDLET, and this is recorded so nobody
#   spends the afternoon again: USkeletonModifier is the engine's bone-renaming
#   API, it is exposed to Python, and its commit path is built on SCustomDialog /
#   FMessageDialog — under -unattended it does not fail, it HANGS, forever.
#   Renaming bones is an interactive-editor operation.
#
#   Adding sockets under the Mannequin's names would have made the suffix
#   irrelevant, and that is not possible either: USkeleton::Sockets and
#   USkeletalMesh::Sockets are protected, and USkeletalMeshSocket's SocketName and
#   BoneName are BlueprintReadOnly, so a socket built from script cannot be named.
#   Skeleton naming is an interactive-editor job in 5.8, full stop.
#
#   So the report carries the truth instead: check_rig proves the mapping is
#   complete — all 24 expected bones present exactly once, nothing extra — and
#   list_attach_points prints the imported name of every bone the game asks for
#   by another name, starting with the "hand_r" that TraceWeaponComponent.cpp
#   attaches the third-person knife to. The physics asset is built after the
#   import rather than by the pipeline, so its bodies are named for the bones as
#   they finally are.
#
# Scale, on the other hand, needs no correction. The FBX declares
# UnitScaleFactor = 1.0 (centimetres) and its bind pose is authored in
# centimetres (head_end at 164.6), so ImportOffsetUniformScale stays at 1.0 and
# the character arrives at its authored size. Compare Scripts/import_pack.py,
# where the glTF pack is in metres and the same knob is ALSO 1.0 because the
# glTF translator has already done the conversion — the number agrees, the
# reason does not.
# =============================================================================
import math
import os
import struct
import zlib

import unreal


PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_FBX = os.path.join(PROJECT_ROOT, "Art", "Characters", "Rocco", "RoccoTest.fbx")
FBX = os.environ.get("TRACE_ROCCO_FBX") or DEFAULT_FBX

ROOT = "/Game/Trace/Characters/Rocco"
STAGING = ROOT + "/_Import"
PIPELINE_DIR = ROOT + "/_Pipelines"

MESH_NAME = "SK_Rocco"
SKELETON_NAME = "SK_Rocco_Skeleton"
# The engine's own name for a mesh's physics asset, kept rather than renamed.
# Renaming it works but leaves a UObjectRedirector at the old path that the next
# run then has to sweep and that a second rename then trips over; and
# "<mesh>_PhysicsAsset" is both Interchange's default and what
# Scripts/import_pack.py already produces for the pack. Two reasons to stop
# fighting it.
PHYSICS_NAME = "SK_Rocco_PhysicsAsset"
MATERIAL_NAME = "M_RoccoPlaceholder"
# One MaterialInstanceConstant per material slot, parented to the placeholder and
# carrying that slot's own Roughness and Metallic (see read_fbx_surfaces). The
# prefix is load-bearing in two places outside this file: the pre-launch wipe in
# Scripts/import-rocco.sh keeps them the way it keeps the placeholder, and its
# idempotence check allows them.
SLOT_MATERIAL_PREFIX = "MI_Rocco_"

MESH_PATH = "{0}/{1}".format(ROOT, MESH_NAME)
SKELETON_PATH = "{0}/{1}".format(ROOT, SKELETON_NAME)
PHYSICS_PATH = "{0}/{1}".format(ROOT, PHYSICS_NAME)
MATERIAL_PATH = "{0}/{1}".format(ROOT, MATERIAL_NAME)

# The yardstick. Every measurement below is printed next to this mesh's, because
# "164 uu tall" only means something beside the body the capsule was tuned for.
MANNEQUIN_PATH = "/Game/Characters/Mannequins/Meshes/SKM_Manny_Simple"

DEFAULT_FBX_PIPELINE = ("/Interchange/Pipelines/DefaultFBXOBJAssetsPipeline"
                        ".DefaultFBXOBJAssetsPipeline")

# The rig this file is expected to contain, in the artist's own spelling. This is
# a CONTRACT, not a convenience: it is what fix_bone_names renames the imported
# bones back to, and any bone that arrives and cannot be matched to it is
# reported rather than quietly accepted.
#
# Note the spine numbering, which reads backwards and is not a typo: Spine02 is
# the LOWEST of the three (it is the Hips' child) and Spine is the highest.
ROOT_BONE = "root"
IMPORTED_ROOT_BONE = "target_character"      # the armature Null, at the origin
EXPECTED_BONES = (
    "Hips",
    "Spine02", "Spine01", "Spine", "neck", "Head", "head_end", "headfront",
    "LeftShoulder", "LeftArm", "LeftForeArm", "LeftHand",
    "RightShoulder", "RightArm", "RightForeArm", "RightHand",
    "LeftUpLeg", "LeftLeg", "LeftFoot", "LeftToeBase",
    "RightUpLeg", "RightLeg", "RightFoot", "RightToeBase",
)

FORCE = os.environ.get("TRACE_FORCE_ROCCO", "0") == "1"

EAL = unreal.EditorAssetLibrary
MEL = unreal.MaterialEditingLibrary

_failures = []


def log(msg):
    unreal.log("[Trace] {0}".format(msg))


def fail(msg):
    _failures.append(msg)
    unreal.log_error("[Trace] {0}".format(msg))


def prop(obj, name, value):
    """set_editor_property, but a rejected name is reported instead of aborting.

    Interchange renames pipeline properties between engine minors more often
    than it changes their meaning, and a silent AttributeError here would come
    back as a mysteriously wrong import rather than as a message."""
    try:
        obj.set_editor_property(name, value)
        return True
    except Exception as exc:                          # noqa: BLE001
        fail("pipeline property {0} = {1!r} rejected: {2}".format(name, value, exc))
        return False


# -----------------------------------------------------------------------------
# Reading the FBX by hand
#
# Interchange is already importing this file, so why open it again? Because the
# two things wanted here are the two things the import is deliberately NOT
# bringing in, and both were previously written down as comments rather than
# measured:
#
#  1. WHETHER THE TEXTURES ARE IN THE FILE. The header says they are not. A
#     comment saying so is a claim that goes stale the moment the artist
#     re-exports; a count of embedded `Content` blocks taken at import time is a
#     measurement that flips by itself and drags the report with it. So the
#     report states the count and lists the external references it found, and
#     "Rocco is grey" stops being something you have to have read the header to
#     know.
#
#  2. THE PER-SLOT SURFACE VALUES. All ten FBX materials carry the SAME 0.8 grey
#     DiffuseColor — Blender's untouched default, because the albedo was coming
#     from the textures that are missing — so there is no colour to recover. But
#     Shininess and ReflectionFactor DIFFER slot to slot, and those are authored
#     numbers: they span roughness 0.52 to 1.00 and put metal on two of the ten
#     slots. Pointing all ten slots at one material threw that away and made
#     Rocco flatter than the file actually says he is. He is still untextured;
#     he is no longer one uniform plastic shell.
#
# The format is documented and stable, and only the node header widens between
# FBX 7400 (this file) and 7500 — everything below handles both.
# -----------------------------------------------------------------------------
_FBX_SCALAR_PROPS = {"Y": ("<h", 2), "C": ("<?", 1), "I": ("<i", 4),
                     "F": ("<f", 4), "D": ("<d", 8), "L": ("<q", 8)}
_FBX_ARRAY_PROPS = {"f": "f", "d": "d", "l": "q", "i": "i", "b": "b"}


def _fbx_prop(data, off, out):
    code = chr(data[off])
    off += 1
    if code in _FBX_SCALAR_PROPS:
        fmt, size = _FBX_SCALAR_PROPS[code]
        out.append(struct.unpack(fmt, data[off:off + size])[0])
        return off + size
    if code in _FBX_ARRAY_PROPS:
        count, encoding, nbytes = struct.unpack("<III", data[off:off + 12])
        off += 12
        raw = data[off:off + nbytes]
        off += nbytes
        if encoding == 1:
            raw = zlib.decompress(raw)
        out.append(list(struct.unpack("<{0}{1}".format(count, _FBX_ARRAY_PROPS[code]), raw))
                   if count else [])
        return off
    if code in ("S", "R"):
        nbytes = struct.unpack("<I", data[off:off + 4])[0]
        off += 4
        out.append(data[off:off + nbytes])
        return off + nbytes
    raise ValueError("unknown FBX property code {0!r} at byte {1}".format(code, off - 1))


def _fbx_node(data, off, wide):
    """One record as (name, props, children), plus the offset just past it.

    A record whose EndOffset is zero is the null terminator that closes a list of
    siblings, and is returned as None."""
    if wide:
        end, nprops, _len = struct.unpack("<QQQ", data[off:off + 24])
        off += 24
    else:
        end, nprops, _len = struct.unpack("<III", data[off:off + 12])
        off += 12
    name_len = data[off]
    off += 1
    name = data[off:off + name_len].decode("utf8", "replace")
    off += name_len
    if end == 0:
        return None, off
    props = []
    for _ in range(nprops):
        off = _fbx_prop(data, off, props)
    kids = []
    while off < end:
        kid, off = _fbx_node(data, off, wide)
        if kid is None:
            break
        kids.append(kid)
    return (name, props, kids), end


def _fbx_name(props):
    """An FBX object's name, which is stored as "name\\x00\\x01Class"."""
    return props[1].split(b"\x00")[0].decode("utf8", "replace") if len(props) > 1 else ""


class FbxSurfaces(object):
    """What read_fbx_surfaces measured. Empty and harmless if the read failed."""

    def __init__(self):
        self.version = 0
        self.embedded_images = 0        # `Content` blocks with bytes in them
        self.external_refs = []         # texture paths the file points at instead
        self.order = []                 # material names, in the kept model's slot order
        self.shininess = {}             # material name -> FBX Shininess
        self.reflection = {}            # material name -> FBX ReflectionFactor
        self.diffuse = {}               # material name -> FBX DiffuseColor, as (r, g, b)

    def distinct_diffuse(self):
        """How many different DiffuseColors the file's materials carry.

        One means there is no per-slot colour to recover and the placeholder is
        the whole story; anything else means somebody authored albedo in the
        materials themselves and this script is throwing it away. Counted rather
        than claimed, because the report asserts the answer in capital letters
        and an assertion nobody re-measures is how the last one went stale."""
        return len({tuple(round(c, 4) for c in v) for v in self.diffuse.values()})

    def roughness(self, material):
        """Blender's own inverse of the mapping it exported with.

        io_scene_fbx writes Shininess = ((1 - roughness) * 10) ** 2 and reads it
        back as 1 - sqrt(Shininess) / 10, so this is a round-trip and not a
        guess at somebody else's convention — but it IS the one assumption in
        this section, so the report prints the raw Shininess beside every
        derived number. If a future exporter changes the mapping, the two
        columns stop agreeing in public instead of quietly shading Rocco wrong."""
        s = max(0.0, float(self.shininess.get(material, 0.0)))
        return max(0.0, min(1.0, 1.0 - math.sqrt(s) / 10.0))

    def metallic(self, material):
        """ReflectionFactor IS Blender's Metallic — a straight 1:1 on both sides
        of io_scene_fbx, with no formula in between."""
        return max(0.0, min(1.0, float(self.reflection.get(material, 0.0))))


def read_fbx_surfaces(path):
    """Measure the source file. Never raises: a read that fails is reported.

    A failure here is a PROBLEM and not a shrug, because the fallback — every
    slot back on one flat material — is precisely the state this function exists
    to end, and it would look identical to success."""
    out = FbxSurfaces()
    try:
        with open(path, "rb") as handle:
            data = handle.read()
        if not data.startswith(b"Kaydara FBX Binary"):
            fail("{0} is not a binary FBX, so its per-slot surface values cannot be "
                 "read".format(path))
            return out
        out.version = struct.unpack("<I", data[23:27])[0]
        wide = out.version >= 7500

        roots = []
        off = 27
        # The 160-byte footer is not a record. Stopping on the null terminator is
        # the primary exit; the length guard is what keeps a truncated file from
        # walking off the end.
        while off < len(data) - 160:
            node, off = _fbx_node(data, off, wide)
            if node is None:
                break
            roots.append(node)

        objects = next((n for n in roots if n[0] == "Objects"), None)
        connections = next((n for n in roots if n[0] == "Connections"), None)
        if objects is None or connections is None:
            fail("{0} has no Objects/Connections section; it is not an FBX this script "
                 "understands".format(path))
            return out

        for child in objects[2]:
            if child[0] == "Material":
                name = _fbx_name(child[1])
                for props70 in (c for c in child[2] if c[0] == "Properties70"):
                    for entry in props70[2]:
                        key = entry[1][0].decode("utf8", "replace")
                        if key == "Shininess" and len(entry[1]) > 4:
                            out.shininess[name] = entry[1][4]
                        elif key == "ReflectionFactor" and len(entry[1]) > 4:
                            out.reflection[name] = entry[1][4]
                        elif key == "DiffuseColor" and len(entry[1]) > 6:
                            out.diffuse[name] = tuple(entry[1][4:7])
            elif child[0] == "Video":
                content = next((c for c in child[2] if c[0] == "Content"), None)
                if content is not None and content[1] and content[1][0]:
                    out.embedded_images += 1
                for c in child[2]:
                    if c[0] == "RelativeFilename" and c[1]:
                        out.external_refs.append(c[1][0].decode("utf8", "replace"))

        # Slot ORDER, not just the set of names. LayerElementMaterial indexes the
        # model's material connections in the order they appear here, so this is
        # the order the ten slots arrive in. The model with the most materials is
        # the finished one — the same rule pick_rocco uses on the imported side,
        # applied to the same file, so the two cannot disagree about which twin
        # this is.
        by_id = {}
        for child in objects[2]:
            if child[1] and isinstance(child[1][0], int):
                by_id[child[1][0]] = (child[0], _fbx_name(child[1]))
        per_model = {}
        for conn in connections[2]:
            p = conn[1]
            if len(p) >= 3 and p[0] == b"OO" and p[1] in by_id and p[2] in by_id:
                src, dst = by_id[p[1]], by_id[p[2]]
                if src[0] == "Material" and dst[0] == "Model":
                    per_model.setdefault(dst[1], []).append(src[1])
        if per_model:
            out.order = max(per_model.values(), key=len)
    except Exception as exc:                          # noqa: BLE001
        fail("could not read the surface values out of {0}: {1}. Every slot falls back to "
             "the flat placeholder.".format(path, exc))
    return out


# -----------------------------------------------------------------------------
# One material instance per slot
#
# The parent below is the placeholder and stays the placeholder: these instances
# add nothing that is not in the FBX. They exist so that the ten slots are ten
# surfaces again rather than ten references to one, which is both what the file
# says and what a later textured re-import needs — a slot that already has its
# own instance is a slot the artist can drop a texture into without touching
# anything else.
#
# THEY ARE UPDATED IN PLACE, NOT REBUILT. Scripts/import-rocco.sh keeps them
# across a normal run for the same reason it keeps M_RoccoPlaceholder: they are
# authored here rather than imported, and churning ten binary LFS assets' GUIDs
# on every run buys nothing. A re-export with different values still lands,
# because the parameters are written every time.
# -----------------------------------------------------------------------------
def build_slot_instances(parent, surfaces, slots, lines):
    """A MaterialInstanceConstant per slot. Returns [MaterialInterface] per slot.

    Slots are matched to FBX materials BY NAME first and by position only as a
    fallback, and which of the two happened is printed. Interchange does not
    promise to keep the FBX's material order, and a positional match that
    silently drifted would put the metal on the wrong part of the body — visible,
    plausible, and impossible to trace back here."""
    if parent is None or not slots:
        return []

    def key(text):
        return "".join(ch for ch in str(text).lower() if ch.isalnum())

    by_key = {}
    for name in surfaces.order:
        by_key.setdefault(key(name), name)

    matched = [by_key.get(key(s)) for s in slots]
    how = "by name"
    if any(m is None for m in matched):
        if len(surfaces.order) == len(slots):
            matched = list(surfaces.order)
            how = "BY POSITION (the slot names do not match the FBX material names)"
        else:
            fail("{0} of {1} slots have no FBX material of the same name and the counts "
                 "differ ({2} materials in the file), so the per-slot surface values cannot "
                 "be placed. Every slot falls back to the flat placeholder."
                 .format(sum(1 for m in matched if m is None), len(slots),
                         len(surfaces.order)))
            return []

    tools = unreal.AssetToolsHelpers.get_asset_tools()
    out = []
    lines.append("per-slot surface values, recovered from the FBX and matched {0}:".format(how))
    lines.append("   {0:<22} {1:<16} {2:>10} {3:>10} {4:>10}".format(
        "slot", "FBX material", "Shininess", "Roughness", "Metallic"))
    for slot, material in zip(slots, matched):
        asset_name = SLOT_MATERIAL_PREFIX + "".join(
            ch if (ch.isalnum() or ch == "_") else "_" for ch in str(slot))
        path = "{0}/{1}".format(ROOT, asset_name)
        mic = unreal.load_asset(path) if EAL.does_asset_exist(path) else None
        if mic is None:
            mic = tools.create_asset(asset_name, ROOT, unreal.MaterialInstanceConstant,
                                     unreal.MaterialInstanceConstantFactoryNew())
        if mic is None:
            fail("AssetTools refused to create {0}".format(path))
            return []
        # Re-parented every run rather than only at creation: --force rebuilds
        # M_RoccoPlaceholder as a NEW object, and an instance still pointing at
        # the old one renders as the grey checkerboard.
        MEL.set_material_instance_parent(mic, parent)
        rough, metal = surfaces.roughness(material), surfaces.metallic(material)
        MEL.set_material_instance_scalar_parameter_value(mic, "Roughness", rough)
        MEL.set_material_instance_scalar_parameter_value(mic, "Metallic", metal)
        EAL.save_loaded_asset(mic, only_if_is_dirty=False)
        out.append(mic)
        lines.append("   {0:<22} {1:<16} {2:>10.3f} {3:>10.3f} {4:>10.3f}".format(
            str(slot)[:22], material[:16], surfaces.shininess.get(material, 0.0),
            rough, metal))
    return out


# -----------------------------------------------------------------------------
# The placeholder material
#
# Not "a grey material" for its own sake: it exists so the untextured import is
# visibly a PLACEHOLDER and not mistaken for finished art, and so the next step
# has a parameter to drive. BaseColor and Tint are separate on purpose — team
# colour goes on Tint from a MID, leaving BaseColor free for the real albedo
# when the textures turn up.
# -----------------------------------------------------------------------------
def build_material():
    if EAL.does_asset_exist(MATERIAL_PATH):
        if not FORCE:
            log("{0} already exists; keeping it (--force rebuilds)".format(MATERIAL_PATH))
            return unreal.load_asset(MATERIAL_PATH)
        EAL.delete_asset(MATERIAL_PATH)

    tools = unreal.AssetToolsHelpers.get_asset_tools()
    mat = tools.create_asset(MATERIAL_NAME, ROOT, unreal.Material, unreal.MaterialFactoryNew())
    if mat is None:
        fail("AssetTools refused to create {0}".format(MATERIAL_PATH))
        return None

    mat.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_DEFAULT_LIT)
    mat.set_editor_property("blend_mode", unreal.BlendMode.BLEND_OPAQUE)
    mat.set_editor_property("two_sided", False)
    # REQUIRED, silent in the editor and fatal in -game. Without
    # MATUSAGE_SkeletalMesh the skinned vertex-factory permutation is never
    # compiled, the renderer substitutes the grey checkerboard, and Rocco shows
    # up to other players as a default-material blob. The editor's auto-repair
    # for this flag is gated on not running as a game, and every run of this
    # project is -game. Same trap as M_TraceNeon in Scripts/generate_content.py.
    mat.set_editor_property("used_with_skeletal_mesh", True)

    def vector(name, value, x, y, priority):
        e = MEL.create_material_expression(mat, unreal.MaterialExpressionVectorParameter, x, y)
        e.set_editor_property("parameter_name", name)
        e.set_editor_property("default_value", value)
        e.set_editor_property("group", "Trace|Rocco")
        e.set_editor_property("sort_priority", priority)
        return e

    def scalar(name, value, x, y, priority):
        e = MEL.create_material_expression(mat, unreal.MaterialExpressionScalarParameter, x, y)
        e.set_editor_property("parameter_name", name)
        e.set_editor_property("default_value", float(value))
        e.set_editor_property("group", "Trace|Rocco")
        e.set_editor_property("sort_priority", priority)
        return e

    def link(src, dst_property):
        if not MEL.connect_material_property(src, "", dst_property):
            fail("could not connect an expression to {0}".format(dst_property))

    base = vector("BaseColor", unreal.LinearColor(0.28, 0.29, 0.32, 1.0), -640, -240, 0)
    tint = vector("Tint", unreal.LinearColor(1.0, 1.0, 1.0, 1.0), -640, -60, 1)
    mul = MEL.create_material_expression(mat, unreal.MaterialExpressionMultiply, -360, -160)
    if not MEL.connect_material_expressions(base, "", mul, "A") \
            or not MEL.connect_material_expressions(tint, "", mul, "B"):
        fail("could not build the BaseColor x Tint multiply")
    link(mul, unreal.MaterialProperty.MP_BASE_COLOR)
    link(scalar("Metallic", 0.0, -640, 120, 2), unreal.MaterialProperty.MP_METALLIC)
    link(scalar("Roughness", 0.62, -640, 240, 3), unreal.MaterialProperty.MP_ROUGHNESS)

    MEL.recompile_material(mat)
    EAL.save_loaded_asset(mat, only_if_is_dirty=False)
    log("Built {0}".format(MATERIAL_PATH))
    return mat


# -----------------------------------------------------------------------------
# The pipeline
#
# FImportAssetParameters::OverridePipelines is a TArray<FSoftObjectPath>, so the
# override has to be a real asset on disk. A configured copy of the engine's FBX
# pipeline is duplicated into the Rocco folder and deleted again at the end of
# the run — same shape as Scripts/import_pack.py.
# -----------------------------------------------------------------------------
def build_pipeline():
    path = "{0}/{1}".format(PIPELINE_DIR, "TraceRoccoFBX")
    if EAL.does_asset_exist(path):
        EAL.delete_asset(path)

    src = unreal.load_asset(DEFAULT_FBX_PIPELINE)
    if src is None:
        fail("could not load {0}".format(DEFAULT_FBX_PIPELINE))
        return None

    pipe = unreal.AssetToolsHelpers.get_asset_tools().duplicate_asset(
        "TraceRoccoFBX", PIPELINE_DIR, src)
    if pipe is None:
        fail("AssetTools refused to duplicate the FBX pipeline into {0}".format(PIPELINE_DIR))
        return None

    # 1.0, and that is not us ignoring a scale problem — see the header. The FBX
    # says UnitScaleFactor = 1.0 and its bind pose is authored in centimetres,
    # so one file unit is already one Unreal unit.
    prop(pipe, "import_offset_uniform_scale", 1.0)
    # Name assets after their MESH NODE, not after the file: this file holds two
    # meshes, and naming both after the source would collide. Everything is
    # renamed to the contract names afterwards anyway; what matters here is that
    # the two arrive distinguishable.
    prop(pipe, "use_source_name_for_asset", False)
    prop(pipe, "scene_name_sub_folder", False)
    prop(pipe, "asset_type_sub_folders", False)

    common = pipe.get_editor_property("common_meshes_properties")
    # The file is genuinely skinned (2 Skin deformers, 24 clusters each), so
    # nothing needs forcing — unlike the pack's GLBs, which report skins: 0.
    prop(common, "force_all_mesh_as_type", unreal.InterchangeForceMeshType.IFMT_NONE)
    prop(common, "bake_meshes", True)
    prop(common, "import_sockets", True)
    # False: the ten slots are ten DIFFERENT materials on one body, not ten
    # copies of one. Merging them would collapse the head, the jacket and the
    # boots into a single section and throw away the only handle a later,
    # textured re-import has to land on.
    prop(common, "keep_sections_separate", False)
    # The mesh carries a greyscale vertex-colour layer named "Attribute", kept
    # because throwing it away would be irreversible and it costs nothing.
    #
    # IT IS NOT AN AMBIENT-OCCLUSION BAKE, which is what this comment used to
    # claim, and the difference matters because an AO layer would be the obvious
    # thing to multiply into the placeholder to stop Rocco reading as one flat
    # shell. Measured: ByPolygonVertex / IndexToDirect, 12516 indices into 48
    # entries, all r == g == b, spread EVENLY from 0.00 to 1.00 with a mean of
    # 0.498 and a near-uniform histogram. Occlusion clusters near white; a flat
    # 0-to-1 spread is a mask or a per-island id. Multiplying it into base colour
    # would darken half the body to black — inventing art, and ugly art. So
    # nothing reads it, on purpose.
    prop(common, "vertex_color_import_option",
         unreal.InterchangeVertexColorImportOption.IVCIO_REPLACE)

    skelcommon = pipe.get_editor_property("common_skeletal_meshes_and_animations_properties")
    prop(skelcommon, "import_only_animations", False)
    # There is no AnimStack in this FBX, so there is no frame 0 to take a ref
    # pose from other than the bind pose. Leave it off and keep the bind pose.
    prop(skelcommon, "use_t0_as_ref_pose", False)

    mesh = pipe.get_editor_property("mesh_pipeline")
    prop(mesh, "import_static_meshes", False)
    prop(mesh, "import_skeletal_meshes", True)
    # DO NOT COMBINE, which is the opposite of what import_pack.py wants and for
    # the opposite reason. The two armatures in this file have IDENTICAL bone
    # names but different orientations, so "combine by skeleton" would happily
    # weld the finished model to the bare base mesh and produce one asset
    # containing both bodies. Keeping them apart is what makes the twin
    # deletable.
    prop(mesh, "combine_skeletal_meshes_behavior",
         unreal.InterchangeCombineSkeletalMeshesBehavior.DO_NOT_COMBINE)
    # Off HERE, and built by hand later — not a decision against having one. A
    # PhysicsAsset names its bodies after the bones it wraps, and the bones this
    # import produces are wrong until fix_bone_names has run, so a pipeline-built
    # asset would ship a body called Hips1 forever. main() calls
    # SkeletalMeshEditorSubsystem.create_physics_asset after the rename instead.
    prop(mesh, "create_physics_asset", False)
    prop(mesh, "build_nanite", False)

    anim = pipe.get_editor_property("animation_pipeline")
    # Nothing to import: the FBX has zero AnimStacks. Off rather than on-and-
    # empty so a future file that DOES carry motion produces a visible change
    # here rather than sneaking clips in unnoticed.
    prop(anim, "import_animations", False)

    mat = pipe.get_editor_property("material_pipeline")
    # Off, because there is nothing worth importing — see the header. Ten
    # identical 0.8-grey Blender materials and six texture references to a
    # Windows path that does not exist on any machine here.
    prop(mat, "import_materials", False)
    tex = mat.get_editor_property("texture_pipeline")
    if tex is not None:
        prop(tex, "import_textures", False)

    EAL.save_loaded_asset(pipe, only_if_is_dirty=False)
    log("Pipeline {0} ready".format(path))
    return path


# -----------------------------------------------------------------------------
# Import, then keep the right half of the file
# -----------------------------------------------------------------------------
def list_folder(folder):
    if not EAL.does_directory_exist(folder):
        return {}
    out = {}
    for p in EAL.list_assets(folder, recursive=True, include_folder=False):
        p = p.split(".")[0]
        asset = unreal.load_asset(p)
        out[p] = asset
    return out


def sweep():
    """Clear anything left over INSIDE the editor session.

    THE REAL SWEEP HAPPENS IN THE SHELL, BEFORE THE EDITOR STARTS, and that is
    not tidiness — it is the only thing that makes a second run work. Deleting
    an asset from Python does remove the file, but the Asset Registry keeps the
    entry for the rest of the session: right after a successful delete_asset,
    does_asset_exist still answers True, and renaming the freshly imported mesh
    onto that name then fails. What that looked like was worse than a failure —
    the rename failed, load_asset handed back the still-resident OLD mesh, and
    the run happily saved and measured the PREVIOUS run's asset. A second run
    was not re-importing at all, and nothing about it looked wrong.

    So Scripts/import-rocco.sh deletes the .uasset files on disk before the
    editor is launched, exactly as a fresh clone would have them, and by the
    time this runs the destination names have never existed in this session.
    What is left here is the in-session scratch: the staging and pipeline
    folders a crashed earlier run may have left on disk."""
    for folder in (STAGING, PIPELINE_DIR):
        if EAL.does_directory_exist(folder):
            EAL.delete_directory(folder)
    present = sorted(list_folder(ROOT))
    log("shell left {0} asset(s) in {1}: {2}"
        .format(len(present), ROOT, ", ".join(p.rsplit("/", 1)[-1] for p in present) or "none"))
    for path in present:
        # The placeholder and its per-slot instances are authored here rather
        # than imported, so the shell leaves them alone on a normal run and they
        # are expected to be standing. Nothing else should be.
        if path != MATERIAL_PATH and not path.rsplit("/", 1)[-1].startswith(SLOT_MATERIAL_PREFIX):
            fail("{0} survived the shell's pre-launch sweep; the rename below will fail. "
                 "Check the wipe step in Scripts/import-rocco.sh.".format(path))


def slot_names(mesh):
    try:
        return [str(s.get_editor_property("material_slot_name"))
                for s in mesh.get_editor_property("materials")]
    except Exception as exc:                          # noqa: BLE001
        fail("{0}: cannot read material slots: {1}".format(mesh.get_path_name(), exc))
        return []


def bone_count(mesh):
    """Bone count off the Skeleton.

    Only the count: FBoneNode exposes no properties to Python (a bone_tree entry
    prints as an empty struct), so the NAMES have to come from a component —
    see probe()."""
    skel = mesh.get_editor_property("skeleton")
    if skel is None:
        return 0
    try:
        return len(skel.get_editor_property("bone_tree"))
    except Exception:                                 # noqa: BLE001
        return 0


def vert_count(mesh):
    try:
        return unreal.get_editor_subsystem(unreal.SkeletalMeshEditorSubsystem).get_num_verts(mesh, 0)
    except Exception:                                 # noqa: BLE001
        return -1


def pick_rocco(landed):
    """Of the skeletal meshes that landed, return the finished model.

    The rule and its justification are in the header: the finished model is the
    one wearing the FBX's ten material slots; the twin has no material layer at
    all and the engine hands it a single unnamed default slot. So the test is
    "many slots versus one", not "some versus none" — that distinction cost a
    run to learn.

    Both arms are checked, because a rule whose two arms are never compared is
    not a rule. If a future re-export gives both meshes real materials, or
    strips them from both, this says so instead of keeping whichever sorted
    first."""
    meshes = [(p, a) for p, a in sorted(landed.items())
              if isinstance(a, unreal.SkeletalMesh)]
    if not meshes:
        fail("no SkeletalMesh arrived from {0}".format(FBX))
        return None, []

    scored = sorted(((p, a, len(slot_names(a))) for p, a in meshes),
                    key=lambda s: (-s[2], s[0]))
    for p, a, n in scored:
        log("  candidate {0}: {1} material slot(s), {2} bone(s), {3} render verts"
            .format(p, n, bone_count(a), vert_count(a)))

    keep = scored[0]
    if keep[2] < 2:
        fail("no skeletal mesh in this FBX carries more than one material slot, so the "
             "finished model cannot be told from the bare twin. Re-read the header.")
        return None, [p for p, _a, _n in scored[1:]]
    if len(scored) > 1 and scored[1][2] > 1:
        fail("{0} skeletal meshes carry more than one material slot; the selection rule no "
             "longer separates them. Re-read the header."
             .format(sum(1 for s in scored if s[2] > 1)))
    if len(scored) < 2:
        fail("the bare twin mesh was expected alongside the finished one and did not arrive; "
             "the selection rule no longer separates anything.")

    return keep[1], [p for p, _a, _n in scored[1:]]


# -----------------------------------------------------------------------------
# The rig, checked — and given the attach points the game already asks for
# -----------------------------------------------------------------------------
def check_rig(mesh, lines):
    """Match the imported bones against EXPECTED_BONES and return the mapping.

    THE BONES DO NOT ARRIVE UNDER THEIR OWN NAMES, and nothing here can change
    that. The FBX holds two armatures sharing all 24 bone names (header, point
    1), so the FBX translator uniquifies the second one it reaches — Rocco's —
    and every bone lands with one character appended: Hips1, LeftHand1,
    Spine011, head_end1. The root is the armature Null, "target_character".

    That was tried and rejected, so it does not get tried again: USkeletonModifier
    is the engine's bone-renaming API and it renames fine, but its commit path
    is built around SCustomDialog/FMessageDialog and it HANGS a -unattended
    commandlet indefinitely. Renaming bones is an interactive-editor operation.

    So the suffix stays. What this function can do is prove the mapping is
    complete — every expected bone present exactly once, nothing extra — and
    hand the exact imported name of every bone the game asks for to whoever
    reads the report (list_attach_points, below).

    The match strips exactly the ONE character the translator appended, rather
    than trailing digits, because Spine01 and Spine02 legitimately end in digits
    and a greedy strip would eat them."""
    imported = current_bone_names(mesh)
    if not imported:
        fail("could not read the imported bone names, so the rig cannot be checked")
        return {}

    wanted = set(EXPECTED_BONES)
    mapping = {}                                      # expected name -> imported name
    unknown = []
    root_as = None
    for name in imported:
        if name == IMPORTED_ROOT_BONE or name == ROOT_BONE:
            root_as = name
        elif name in wanted:
            mapping[name] = name
        elif len(name) > 1 and name[:-1] in wanted:
            mapping[name[:-1]] = name
        else:
            unknown.append(name)

    if unknown:
        fail("{0} imported bone(s) match nothing in EXPECTED_BONES: {1}"
             .format(len(unknown), ", ".join(unknown)))
    missing = sorted(wanted - set(mapping))
    if missing:
        fail("{0} expected bone(s) did not arrive: {1}".format(len(missing), ", ".join(missing)))
    if root_as is None:
        fail("no root bone arrived; expected {0!r} or {1!r}"
             .format(IMPORTED_ROOT_BONE, ROOT_BONE))

    suffixed = sorted(k for k, v in mapping.items() if k != v)
    if not missing and not unknown and root_as is not None:
        lines.append("  rig            all {0} expected bones present, root is {1!r}"
                     .format(len(EXPECTED_BONES), root_as))
    if suffixed:
        lines.append("  NAME SUFFIX    {0} bone(s) carry a trailing character the FBX translator"
                     .format(len(suffixed)))
        lines.append("                 added to break a name collision with the twin armature,")
        lines.append("                 e.g. {0}."
                     .format(", ".join("{0}->{1}".format(k, mapping[k]) for k in suffixed[:3])))
        lines.append("                 Renaming them needs the interactive editor, so anything that")
        lines.append("                 names a bone must use the RIGHT-HAND column below, not the")
        lines.append("                 name the artist gave it.")
    return mapping


# WHAT THE GAME ASKS FOR BY NAME, AND WHY IT IS NOT HERE
#   Source/Trace/Gameplay/TraceWeaponComponent.cpp attaches the third-person
#   knife to a socket called "hand_r", behind a DoesSocketExist guard, so on a
#   mesh without one a Rocco player carries an INVISIBLE knife. The obvious fix
#   is to add sockets named for the Mannequin's bones. It cannot be done from
#   here, and this is written down so the next person does not rediscover it:
#
#     USkeleton::Sockets and USkeletalMesh::Sockets are both protected, so
#     get_editor_property("sockets") throws outright; and USkeletalMesh::AddSocket
#     IS exposed, but USkeletalMeshSocket::SocketName and ::BoneName are
#     VisibleAnywhere + BlueprintReadOnly, so a socket built from script cannot be
#     given a name to add.
#
#   Between that and USkeletonModifier hanging (check_rig), authoring a skeleton's
#   naming is an interactive-editor job in 5.8. Two ways forward, both outside
#   this script: add the sockets by hand in the Skeleton editor, or have the
#   gameplay code ask for the bone this rig actually has. list_attach_points()
#   prints exactly what to type either way.
def list_attach_points(mapping, lines):
    lines.append("  ATTACH POINTS this rig offers, against the names the game uses today:")
    lines.append("    {0:<12} {1:<16} {2}".format("game asks", "Rocco has", "used by"))
    for socket_name, expected_bone, user in (
            ("hand_r", "RightHand", "TraceWeaponComponent.cpp — third-person knife, "
                                    "and Trace.DebugAnimProbe"),
            ("hand_l", "LeftHand", "-"),
            ("foot_l", "LeftFoot", "Trace.DebugAnimProbe"),
            ("foot_r", "RightFoot", "-"),
            ("head", "Head", "-"),
            ("pelvis", "Hips", "-")):
        lines.append("    {0:<12} {1:<16} {2}".format(
            socket_name, mapping.get(expected_bone, "<MISSING>"), user))
    lines.append("    None of these exist as SOCKETS — they are bone names. Adding sockets from")
    lines.append("    script is not possible in 5.8 (see the comment above this function).")


def current_bone_names(mesh):
    """Bone names, in index order, read off a throwaway component."""
    world = editor_world()
    if world is None:
        return []
    subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actor = subsystem.spawn_actor_from_class(
        unreal.SkeletalMeshActor, unreal.Vector(0, 0, 0), unreal.Rotator(0, 0, 0))
    if actor is None:
        return []
    try:
        comp = actor.skeletal_mesh_component
        comp.set_skeletal_mesh_asset(mesh)
        return [str(comp.get_bone_name(i)) for i in range(comp.get_num_bones())]
    except Exception as exc:                          # noqa: BLE001
        fail("could not read bone names: {0}".format(exc))
        return []
    finally:
        subsystem.destroy_actor(actor)


def rename(src, dst):
    if src == dst:
        return dst
    if EAL.does_asset_exist(dst):
        EAL.delete_asset(dst)
    if EAL.rename_asset(src, dst):
        return dst
    fail("could not rename {0} -> {1}".format(src, dst))
    return src


def do_import(pipeline_path):
    im = unreal.InterchangeManager.get_interchange_manager_scripted()
    source = unreal.InterchangeManager.create_source_data(FBX)
    params = unreal.ImportAssetParameters()
    params.set_editor_property("is_automated", True)
    try:
        params.set_editor_property("override_pipelines", [unreal.SoftObjectPath(pipeline_path)])
    except Exception as exc:                          # noqa: BLE001
        fail("override_pipelines rejected ({0}); falling back to a plain string".format(exc))
        params.set_editor_property("override_pipelines", [pipeline_path])
    result = im.import_asset(STAGING, source, params)
    log("import_asset returned {0!r}".format(result))
    return result


# -----------------------------------------------------------------------------
# Measurement
#
# Everything downstream reads this block, so it prints numbers that were read
# back off the imported asset — never the numbers this script asked for.
#
# Bone positions need a component to hang off: a Skeleton's bone_tree gives
# names and nothing else, and there is no reference-pose accessor on it from
# Python. So one SkeletalMeshActor is spawned in the editor's world at the
# origin with no rotation, read, and destroyed. The level is never saved.
# -----------------------------------------------------------------------------
KEY_BONES = ("Hips", "Spine", "Spine01", "Spine02", "neck", "Head", "head_end", "headfront",
             "LeftShoulder", "LeftArm", "LeftForeArm", "LeftHand",
             "RightShoulder", "RightArm", "RightForeArm", "RightHand",
             "LeftUpLeg", "LeftLeg", "LeftFoot", "LeftToeBase",
             "RightUpLeg", "RightLeg", "RightFoot", "RightToeBase",
             # The Mannequin's equivalents, so one call site measures both rigs.
             "root", "pelvis", "spine_01", "spine_05", "neck_01", "head",
             "clavicle_l", "upperarm_l", "lowerarm_l", "hand_l",
             "clavicle_r", "upperarm_r", "lowerarm_r", "hand_r",
             "thigh_l", "calf_l", "foot_l", "ball_l",
             "thigh_r", "calf_r", "foot_r", "ball_r")


def editor_world():
    try:
        return unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    except Exception:                                 # noqa: BLE001
        try:
            return unreal.EditorLevelLibrary.get_editor_world()
        except Exception:                             # noqa: BLE001
            return None


def probe(mesh, label, lines, extra=()):
    """Spawn, measure, destroy. Returns the bone table so callers can compare."""
    lines.append("{0}: {1}".format(label, mesh.get_path_name().split(".")[0]))
    for line in extra:
        lines.append(line)

    try:
        b = mesh.get_bounds()
        o, e = b.origin, b.box_extent
        lines.append("  asset bounds   origin=({0:7.2f},{1:7.2f},{2:7.2f})  "
                     "extent=({3:7.2f},{4:7.2f},{5:7.2f}) uu  "
                     "-> size ({6:.2f} x {7:.2f} x {8:.2f}) uu"
                     .format(o.x, o.y, o.z, e.x, e.y, e.z, e.x * 2, e.y * 2, e.z * 2))
        lines.append("  Z range        {0:.2f} .. {1:.2f} uu   (feet at 0 means the origin is "
                     "under the soles, hips-at-0 would put it mid-body)"
                     .format(o.z - e.z, o.z + e.z))
    except Exception as exc:                          # noqa: BLE001
        lines.append("  bounds unavailable: {0}".format(exc))

    world = editor_world()
    if world is None:
        lines.append("  NO EDITOR WORLD — bone positions not measured")
        return {}

    subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actor = subsystem.spawn_actor_from_class(
        unreal.SkeletalMeshActor, unreal.Vector(0, 0, 0), unreal.Rotator(0, 0, 0))
    if actor is None:
        lines.append("  could not spawn a SkeletalMeshActor — bone positions not measured")
        return {}

    table = {}
    try:
        comp = actor.skeletal_mesh_component
        comp.set_skeletal_mesh_asset(mesh)
        n = comp.get_num_bones()
        names = [str(comp.get_bone_name(i)) for i in range(n)]
        lines.append("  bones          {0}".format(n))
        lines.append("  root bone      {0!r}".format(names[0] if names else "<none>"))
        for name in names:
            t = comp.get_socket_transform(name, unreal.RelativeTransformSpace.RTS_COMPONENT)
            table[name] = t.translation
        lines.append("  hand_r socket exists on this mesh: {0}   (the third-person knife in "
                     "TraceWeaponComponent.cpp attaches there)"
                     .format(comp.does_socket_exist("hand_r")))
        lines.append("  bone list (component space, uu — X forward, Y right, Z up):")
        for i, name in enumerate(names):
            parent = str(comp.get_parent_bone(name))
            v = table[name]
            star = "*" if name in KEY_BONES else " "
            lines.append("   {0}{1:>3} {2:<16} parent={3:<16} ({4:8.2f},{5:8.2f},{6:8.2f})"
                         .format(star, i, name, parent or "-", v.x, v.y, v.z))
    except Exception as exc:                          # noqa: BLE001
        lines.append("  bone read failed: {0}".format(exc))
    finally:
        subsystem.destroy_actor(actor)
    return table


def mesh_height(path):
    """Total Z size of a skeletal mesh's bounds, in uu."""
    asset = unreal.load_asset(path)
    if asset is None:
        return None
    try:
        return asset.get_bounds().box_extent.z * 2.0
    except Exception:                                 # noqa: BLE001
        return None


def facing(table, left, right, lines, head=None, front=None):
    """Which way this rig looks, measured rather than eyeballed, and the
    TraceCharacterLayout::MeshYaw that turns it to face Unreal's +X.

    The measurement is rig-agnostic on purpose, because the two rigs share no
    bone names: take L, the vector from the right hand to the left hand — a
    T-posed or A-posed body's own left — and the forward direction is Z x L.
    (Unreal is left-handed with X forward, Y right, Z up, so for a body facing
    +X the left hand is at -Y and Z x (-Y) = +X.)

    THE HARNESS IS CHECKED AGAINST A KNOWN ANSWER RATHER THAN TRUSTED: run on
    SKM_Manny_Simple this must print MeshYaw -90, because -90 is the value
    TraceCharacterLayout::MeshYaw has always carried for the Mannequin. If it
    does not, the formula is wrong and Rocco's number is worthless too.

    `front`, when the rig has one, is a marker bone pushed out of the face; the
    head->front vector is an independent second opinion."""
    if left not in table or right not in table:
        return
    lv, rv = table[left], table[right]
    lx, ly = lv.x - rv.x, lv.y - rv.y             # the body's own left, flattened
    fx, fy = -ly, lx                              # Z x L
    span = (fx * fx + fy * fy) ** 0.5
    if span < 1e-3:
        lines.append("  FACES          undeterminable: {0} and {1} are in the same place"
                     .format(left, right))
        return
    fx, fy = fx / span, fy / span
    heading = math.degrees(math.atan2(fy, fx))    # 0 = +X, +90 = +Y
    lines.append("  FACES          ({0:+.3f}, {1:+.3f}) in mesh space = {2:+.1f} deg off +X"
                 "   [from {3} -> {4}, arm span {5:.1f} uu]"
                 .format(fx, fy, heading, right, left, span))
    lines.append("  MeshYaw needed {0:+.1f}   (the yaw that turns this mesh to face the "
                 "capsule's forward; TraceCharacterLayout::MeshYaw is -90 for the Mannequin)"
                 .format(-heading))
    if head and front and head in table and front in table:
        h, f = table[head], table[front]
        lines.append("  cross-check    {0} -> {1} points ({2:+.2f}, {3:+.2f}, {4:+.2f}) — same "
                     "answer if its X/Y agrees with FACES above"
                     .format(head, front, f.x - h.x, f.y - h.y, f.z - h.z))


# -----------------------------------------------------------------------------
def main():
    log("import_rocco.py starting (FORCE={0})".format(FORCE))
    log("source: {0}".format(FBX))
    if not os.path.isfile(FBX):
        fail("source FBX missing: {0}".format(FBX))
        report([])
        return

    # Read the source file before handing it to Interchange. This is what turns
    # "Rocco is untextured" from a comment in the header into a number in the
    # report, and it is where the per-slot Roughness and Metallic come from.
    surfaces = read_fbx_surfaces(FBX)
    log("FBX {0}: {1} material(s), {2} embedded image(s), {3} external texture reference(s)"
        .format(surfaces.version, len(surfaces.order), surfaces.embedded_images,
                len(surfaces.external_refs)))

    # Sweep BEFORE building the material, not after: --force makes the sweep
    # delete the material too, so building it first would build it and then
    # throw it away. (That is not hypothetical — it happened.)
    sweep()
    material = build_material()

    pipeline_path = build_pipeline()
    if pipeline_path is None:
        report([])
        return

    do_import(pipeline_path)
    landed = list_folder(STAGING)
    log("staging holds {0} asset(s):".format(len(landed)))
    for p, a in sorted(landed.items()):
        log("    {0}  [{1}]".format(p, type(a).__name__ if a is not None else "<unloadable>"))

    mesh, discards = pick_rocco(landed)
    if mesh is None:
        report([])
        return

    mesh_src = mesh.get_path_name().split(".")[0]
    skeleton = mesh.get_editor_property("skeleton")
    skeleton_src = skeleton.get_path_name().split(".")[0] if skeleton else None

    log("keeping {0} -> {1}".format(mesh_src, MESH_PATH))
    rename(mesh_src, MESH_PATH)
    if skeleton_src:
        rename(skeleton_src, SKELETON_PATH)
    else:
        fail("the kept mesh has no Skeleton")

    # Whatever is left in staging is the twin and its scaffolding. It has to go
    # BEFORE the bone rename: the twin owns the unsuffixed copies of all 24
    # names, and its Skeleton is what Rocco's bones would collide with.
    for p in discards:
        log("discarding {0} (the bare, material-less twin, or an extra)".format(p))
    if EAL.does_directory_exist(STAGING):
        EAL.delete_directory(STAGING)
    if EAL.does_directory_exist(PIPELINE_DIR):
        EAL.delete_directory(PIPELINE_DIR)

    rig_lines = []
    mesh = unreal.load_asset(MESH_PATH)
    # Re-save the mesh now the skeleton has moved. Its package was written by the
    # rename above, while the skeleton was still in _Import; leaving it there
    # means an interrupted run leaves SK_Rocco pointing at a path that no longer
    # exists, and it loads with a null Skeleton. Cheap insurance, learned the
    # expensive way.
    EAL.save_loaded_asset(mesh, only_if_is_dirty=False)
    mapping = check_rig(mesh, rig_lines)
    list_attach_points(mapping, rig_lines)

    # Now that the bones are called what the artist called them, the ragdoll's
    # bodies will be too. 24 humanoid bones is a sane ragdoll; it is scaffolding
    # for whatever the gameplay side wants, since the shipped character collides
    # through its capsule and not through this.
    subsystem = unreal.get_editor_subsystem(unreal.SkeletalMeshEditorSubsystem)
    physics = None
    try:
        physics = subsystem.create_physics_asset(mesh)
    except Exception as exc:                          # noqa: BLE001
        fail("create_physics_asset failed: {0}".format(exc))
    if physics is not None:
        landed_at = physics.get_path_name().split(".")[0]
        if landed_at != PHYSICS_PATH:
            fail("the physics asset landed at {0}, not {1}; PHYSICS_NAME and the engine's "
                 "naming have drifted apart".format(landed_at, PHYSICS_PATH))
        mesh.set_editor_property("physics_asset", physics)
        EAL.save_loaded_asset(mesh, only_if_is_dirty=False)
        # No body count: UPhysicsAsset::SkeletalBodySetups is not readable from
        # Python, and a "? bodies" in a measurements report is worse than no
        # number at all.
        rig_lines.append("  physics asset  {0} (built after the import, so its bodies are named "
                         "for the bones as imported)".format(PHYSICS_PATH))
    else:
        fail("no PhysicsAsset was created")

    # Each slot wears its OWN instance of the placeholder, carrying that slot's
    # Roughness and Metallic as the FBX authored them. Slot NAMES are untouched:
    # they are the only handle a later textured re-import has, and they are also
    # what build_slot_instances matches on.
    #
    # WRITE EACH MODIFIED SLOT BACK INTO THE ARRAY BY INDEX. Iterating an
    # unreal.Array of USTRUCTs yields COPIES, so the obvious
    #     for slot in arr: slot.set_editor_property("material_interface", m)
    # mutates ten temporaries and changes nothing — and it changes nothing
    # SILENTLY, leaving all ten slots on WorldGridMaterial, which is the grey
    # checkerboard. That is precisely the "the character models were not
    # replaced" failure this project has been bitten by before, so the
    # assignment is read back off the asset below rather than assumed.
    mesh = unreal.load_asset(MESH_PATH)
    surface_lines = []
    slots = slot_names(mesh) if mesh is not None else []
    instances = build_slot_instances(material, surfaces, slots, surface_lines)
    # One arm or the other, never a mixture: if the per-slot instances could not
    # be built the flat placeholder is still better than the checkerboard, and
    # build_slot_instances has already said why it gave up.
    if not instances:
        instances = [material] * len(slots)
        surface_lines.append("per-slot surface values could NOT be recovered; all {0} slots "
                             "fall back to {1}.".format(len(slots), MATERIAL_NAME))

    if material is not None and mesh is not None:
        arr = mesh.get_editor_property("materials")
        for i in range(len(arr)):
            slot = arr[i]
            slot.set_editor_property("material_interface", instances[i])
            arr[i] = slot
        mesh.set_editor_property("materials", arr)
        EAL.save_loaded_asset(mesh, only_if_is_dirty=False)

        # Read back off the SAVED asset, and compare against the instance that
        # slot was supposed to get rather than against "not null" — a run that
        # put the same material on all ten would pass a null check and is the
        # exact regression this section replaced.
        landed_slots = unreal.load_asset(MESH_PATH).get_editor_property("materials")
        wrong = [str(landed_slots[i].get_editor_property("material_slot_name"))
                 for i in range(len(landed_slots))
                 if landed_slots[i].get_editor_property("material_interface") != instances[i]]
        if wrong:
            fail("{0} of {1} material slots did NOT take the material they were given and are "
                 "still on the default checkerboard: {2}"
                 .format(len(wrong), len(slots), ", ".join(wrong)))

    EAL.save_directory(ROOT, only_if_is_dirty=False, recursive=True)

    # ONE LAST SWEEP, AND IT IS NOT BELT-AND-BRACES.
    #   EditorAssetLibrary.rename_asset leaves a UObjectRedirector behind at the
    #   old path, and save_directory then writes it to disk — so a run that moved
    #   the imported Rocco_Model_1 to SK_Rocco can ship a stray
    #   Rocco_Model_1.uasset alongside it. The shell wrapper's idempotence check
    #   caught exactly that, which is what the check is for. Nothing references
    #   the old paths, so the redirectors are deleted outright.
    keep = set((MESH_PATH, SKELETON_PATH, PHYSICS_PATH, MATERIAL_PATH))
    keep.update(i.get_path_name().split(".")[0] for i in instances if i is not None)
    strays = [p for p in sorted(list_folder(ROOT)) if p not in keep]
    for path in strays:
        if EAL.delete_asset(path):
            log("swept stray {0} (a rename redirector)".format(path))
        else:
            fail("could not delete stray asset {0}".format(path))

    # ---------------------------------------------------------------- report
    lines = []
    lines.append("")
    lines.append("material slots ({0}), read back off the saved asset, each on its own instance "
                 "of {1}:".format(len(slots), MATERIAL_PATH))
    lines.extend("    " + line for line in surface_lines)
    lines.append("")

    # THE ALBEDO VERDICT, AND IT IS MEASURED RATHER THAN ASSERTED. Everything on
    # these lines came out of read_fbx_surfaces, so a re-export that finally
    # carries its images changes this paragraph without anybody editing it —
    # which is the point. The previous version of this block was a hand-written
    # note claiming the same thing, and a hand-written note cannot tell you the
    # day it stops being true.
    if surfaces.embedded_images > 0:
        lines.append("TEXTURES: {0} image(s) ARE embedded in this FBX. Material and texture import"
                     .format(surfaces.embedded_images))
        lines.append("          are still switched OFF in build_pipeline() — turn them on and")
        lines.append("          delete the placeholder path; see the header.")
    else:
        lines.append("*** ROCCO IS UNTEXTURED, AND WHAT OTHER PLAYERS SEE IS A FLAT, UNTEXTURED")
        lines.append("*** BODY — the right silhouette, the right skin weights, the right motion,")
        lines.append("*** and no albedo anywhere on it. This is the FILE, not the import:")
        lines.append("      embedded images in the FBX ......... {0}".format(surfaces.embedded_images))
        lines.append("      external texture references ........ {0}".format(len(surfaces.external_refs)))
        for ref in surfaces.external_refs:
            lines.append("          {0}".format(ref))
        distinct = surfaces.distinct_diffuse()
        example = ", ".join("{0:.2f}".format(c)
                            for c in (surfaces.diffuse.get(surfaces.order[0])
                                      if surfaces.order else ())) or "none read"
        lines.append("      distinct DiffuseColor values across all {0} materials ... {1} ({2})"
                     .format(len(surfaces.order), distinct, example))
        if distinct > 1:
            lines.append("      ^ MORE THAN ONE. Somebody authored albedo into the materials")
            lines.append("        themselves and this import is discarding it. build_material()")
            lines.append("        and build_slot_instances() need a BaseColor per slot.")
        lines.append("*** Roughness and Metallic ARE recovered per slot (table above); colour")
        lines.append("*** cannot be, because it is not in the file. THE FIX IS A RE-EXPORT:")
        lines.append("*** Path Mode = Copy with \"Embed Textures\" ticked, or the six PNGs by hand.")
        lines.append("*** The slot names above survive a re-import, so it lands on these slots.")
    lines.append("")

    rocco = probe(unreal.load_asset(MESH_PATH), "ROCCO", lines, rig_lines)
    # Through the mapping, not by literal name: Rocco's bones are Hips1/LeftHand1,
    # and looking them up as "LeftHand" is how this measurement silently printed
    # nothing at all the first time.
    def r(expected):
        return mapping.get(expected, expected)
    facing(rocco, r("LeftHand"), r("RightHand"), lines,
           head=r("Head"), front=r("headfront"))
    lines.append("")

    manny_asset = unreal.load_asset(MANNEQUIN_PATH)
    if manny_asset is None:
        lines.append("MANNEQUIN {0} is not imported on this machine, so there is nothing to "
                     "compare against. Run Scripts/import-mannequin.sh.".format(MANNEQUIN_PATH))
        manny = {}
    else:
        manny = probe(manny_asset, "MANNEQUIN (the yardstick)", lines)
        facing(manny, "hand_l", "hand_r", lines)

    if rocco and manny:
        lines.append("")
        lines.append("SIDE BY SIDE (component space, uu):")
        pairs = [("Hips", "pelvis"), ("Spine", "spine_05"), ("Head", "head"),
                 ("LeftHand", "hand_l"), ("RightHand", "hand_r"),
                 ("LeftFoot", "foot_l"), ("RightFoot", "foot_r"),
                 ("LeftUpLeg", "thigh_l"), ("LeftShoulder", "clavicle_l")]
        lines.append("   {0:<14} {1:<26} {2:<14} {3:<26}".format(
            "Rocco bone", "position", "Mannequin bone", "position"))
        for expected, m in pairs:
            rname = r(expected)
            rv, mv = rocco.get(rname), manny.get(m)
            lines.append("   {0:<14} {1:<26} {2:<14} {3:<26}".format(
                rname, "({0:.1f},{1:.1f},{2:.1f})".format(rv.x, rv.y, rv.z) if rv else "-",
                m, "({0:.1f},{1:.1f},{2:.1f})".format(mv.x, mv.y, mv.z) if mv else "-"))
        # HEIGHT IS COMPARED ON THE MESH BOUNDS, NOT ON A BONE. Rocco's topmost
        # bone is head_end, at the crown; the Mannequin's is "head", inside the
        # skull. Putting those two numbers side by side made Rocco look TALLER
        # than the Mannequin, which is backwards by 16 uu.
        rb, mb = mesh_height(MESH_PATH), mesh_height(MANNEQUIN_PATH)
        if rb and mb:
            lines.append("   HEIGHT (mesh bounds): Rocco {0:.1f} uu vs Mannequin {1:.1f} uu "
                         "-> Rocco is {2:.0f}% of the Mannequin, and {3:.1f} uu shorter than "
                         "the 176 uu capsule (34 r, 88 half-height) the game is tuned around."
                         .format(rb, mb, 100.0 * rb / mb, 176.0 - rb))
        rh = rocco.get(r("head_end"))
        mh = manny.get("head")
        if rh and mh:
            lines.append("   for reference, topmost BONE: Rocco {0} at Z {1:.1f} is the crown; "
                         "Mannequin head at Z {2:.1f} is inside the skull — not comparable, "
                         "which is why the line above uses the bounds."
                         .format(r("head_end"), rh.z, mh.z))

    report(lines)


def report(lines):
    log("")
    log("================ ROCCO IMPORT REPORT ================")
    for line in lines:
        log(line)
    log("")
    if _failures:
        log("{0} PROBLEM(S):".format(len(_failures)))
        for f in _failures:
            log("  - {0}".format(f))
    else:
        log("no problems reported.")
    log("================ END REPORT ================")


main()
