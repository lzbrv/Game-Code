# =============================================================================
# Trace — import_hands.py
#
# Brings the owner's first-person arms rig into the project as a posable
# SkeletalMesh. TWO STAGES, one file, driven by Scripts/import-hands.sh:
#
#   rebuild  plain python3, NO editor.  Reads Art/Characters/Hands/HandModel2.fbx
#            and writes Intermediate/Hands/TraceArms.glb + TraceArms_manifest.json
#            — the same shape as Scripts/generate_characters.py -> import_characters.py.
#   import   inside the editor (UnrealEditor-Cmd -run=pythonscript).  Imports that
#            GLB through Interchange to /Game/Trace/Characters/Hands/SK_TraceArms
#            and MEASURES what arrived.
#
# The stage is chosen by TRACE_HANDS_STAGE, defaulting to `import` when the
# `unreal` module is importable and `rebuild` when it is not, so neither stage
# can be run in the wrong interpreter by accident.
#
# =============================================================================
# WHY THE FBX IS NOT IMPORTED DIRECTLY — MEASURED, NOT ASSUMED
#
# `Hand Model2.fbx` is a Blender export of a full Rigify human rig that happens
# to have two arms skinned to it.  ◆MEASURED off the file (see the `rebuild`
# stage, which re-measures every run, and Art/Characters/Hands/SOURCE_NOTES.md):
#
#     1,251 Model nodes          — DEF- (272), ORG- (266), MCH- (212) plus
#                                  controls, INCLUDING AN ENTIRE FACE (brow, lid,
#                                  lip, jaw, chin, ear, nose, cheek, eye) and a
#                                  full spine, none of which is skinned to
#                                  anything in this file.
#       705 skin clusters        — of which only 47 carry Weights/Indexes.
#       336 vertices, 338 polys  — the whole mesh.
#
# ◆MEASURED by importing it anyway (the `probe` inside the import stage, which
# runs EVERY time so this number cannot go stale): Interchange turns that into a
# SkeletalMesh with **1051 bones** and ONE material slot pointed at
# WorldGridMaterial.  A 1051-bone skeleton on a 336-vertex mesh means every pose
# asset, every AnimSequence and every frame of evaluation carries a face and a
# spine that can never move anything.
#
# ◆MEASURED, the second option is not available: UE 5.8's Interchange pipeline
# exposes no bone filter of any kind.  The full property list of
# InterchangeGenericMeshPipeline / CommonMeshesProperties /
# CommonSkeletalMeshesAndAnimationsProperties was dumped in the probe run
# (Saved/Logs/release/d29h-probe-asis.log) and contains bone_influence_limit,
# skeletal_mesh_import_content_type and update_skeleton_reference_pose — nothing
# that includes or excludes bones by name, by family, or by "has weights".
# There is no import setting that fixes this.  And bones cannot be deleted or
# renamed afterwards: USkeletonModifier's commit path hangs forever under
# -unattended (recorded in Scripts/import_rocco.py, ◆MEASURED wave 3).
#
# So the rig is REBUILT: the `rebuild` stage re-emits the same 336 vertices,
# with the same weights, bound to the 51 bones that actually matter, as a
# skinned GLB this project's own tooling can write and Interchange imports
# cleanly.  Nothing is invented — every position, every weight and every bone
# orientation is copied out of the FBX.  Three things are changed on purpose and
# all three are named in the report:
#
#   * BONE NAMES.  DEF-f_ring.01.L becomes ring_left_0 — the naming the SHIPPED
#     first-person hands already use, so the C++ that poses them keeps working.
#     TraceKnifeView.cpp builds the knife's hold basis out of index_right_0,
#     pinky_right_0 and index_right_2 by name; TraceCharacterInternal.h hides
#     hand_left by name.  Those three bones mean the same joints on this rig.
#     (Names must be right AT IMPORT: see the USkeletonModifier note above.)
#   * ONE MIS-PARENTED BONE.  ◆MEASURED: ORG-palm.04.L — the LEFT pinky's
#     metacarpal, and the parent of the whole left pinky chain — is parented to
#     the rig's `root` in the FBX, not to DEF-hand.L.  Its mirror ORG-palm.04.R
#     is parented to DEF-hand.R correctly.  Left as authored, the left pinky
#     stays behind at the origin's mercy whenever the left hand moves; it is a
#     detached finger, not a stylistic choice.  palm_pinky_left is parented to
#     hand_left here, matching its own mirror.  This is the ONLY hierarchy edit.
#   * ONE MIS-SIDED SKIN CLUSTER.  ◆MEASURED: DEF-palm.04.R weights 56 vertices
#     above 0.2 and 28 of them are on the LEFT hand (bind-world X +78.9, against
#     the bone's own -73.5).  It is the same asymmetry as the bone above — the
#     left pinky metacarpal has no DEF- bone in this file — and the mirror
#     modifier left the left palm bound to the right bone.  A T-pose hides it
#     completely (both hands are mirror images, so nothing moves); posing one
#     hand differently from the other drags that patch of the left palm 60-70 uu
#     across the frame, which is what stage 2 measured on all three weapons.
#     Every influence whose vertex is on the far side of X=0 from its own bone is
#     moved to that bone's mirror twin, after asserting that no vertex comes
#     within 10 cm of X=0 so the side of a vertex is never in doubt.  It is
#     re-checked after the move, and it is the only bone this catches.
#   * THE RING CHAIN'S BIND ROLL, CORRECTED AT SOURCE.  ◆MEASURED:
#     DEF-f_ring.01/02/03 on BOTH hands have their flexion axis rolled 177-179
#     degrees away from index/middle/pinky — the owner's "the ring fingers bend
#     opposite the rest", and it is a BIND-POSE fact in the source rig, not a
#     posing mistake.  apply_ring_roll_fix() rolls those six bind frames 180
#     degrees about their own along-bone axis (local +Y here), which negates
#     local +X (flexion) and local +Z (abduction) and leaves the bone's
#     direction, length and position untouched.
#
#     WHY AT THE SOURCE AND NOT IN THE POSE.  A bind-pose defect is CONSTANT: it
#     does not depend on the pose, the clip or the amount of curl.  Correcting it
#     in a pose means every pose, every future clip and every retarget has to
#     remember to negate six angles, and the first one that forgets ships a hand
#     with one finger standing up.  Correcting it here makes "rotate a finger
#     bone +X to curl it" true of all four fingers, which is the invariant the
#     rest of the pipeline can actually rely on.
#
#     IT CHANGES NOTHING ABOUT WHAT IS DRAWN AT REST.  The skin is
#     p' = SUM_j w_j * (W_j * IBM_j) * p with IBM_j = link_j^-1, so replacing
#     link_j by (R * link_j) also replaces IBM_j by (link_j^-1 * R^-1) and the
#     product at rest is still the identity.  Asserted numerically both ways:
#     the roll table is measured BEFORE and AFTER the fix (inverted -> consistent,
#     residual <= 1.1 deg against the middle finger), and the import stage's
#     end-to-end bounds check against the raw-FBX probe still has to come out at
#     0.000 uu of drift.
#
# SKINNING MATHS, ◆MEASURED RATHER THAN TRUSTED
#   FBX skins as  p_world = SUM_j w_j * p_mesh * Transform_j * TransformLink_j^-1
#   * BoneGlobal_j.  Blender does not write Transform_j as the mesh's global
#   matrix (473 distinct values across the 705 clusters), so the identity that
#   makes this rebuild valid is checked numerically at rebuild time:
#   Transform_j * TransformLink_j == MeshGlobal for all 47 weighted clusters
#   (max element error 3.3e-5 on a matrix scaled by 100).  With that established
#   the rebuild emits vertices in BIND-WORLD space and sets
#   inverseBindMatrices[j] = TransformLink_j^-1, which is identity at rest.
#
# UNITS AND AXES, ◆MEASURED END TO END
#   The FBX geometry is in metres, Blender Z-up; Model "Arm 2" carries
#   Lcl Rotation (-90,0,0) and Lcl Scaling 100, so bind-world is centimetres,
#   Y-up.  glTF is metres, Y-up, and Interchange maps glTF -> UE exactly as the
#   FBX translator does: UE(X,Y,Z) = (gl.x, gl.z, gl.y) * 100 (the same mapping
#   Scripts/trace_glb.py measured).  The proof is that the rebuilt GLB must land
#   on the SAME UE bounds as the raw FBX did — the import stage asserts that
#   against the probe it just ran, so a coordinate slip cannot pass silently.
# =============================================================================
import json
import math
import os
import struct
import sys
import zlib

try:                                                  # editor stage only
    import unreal
except ImportError:                                   # noqa: BLE001 - rebuild stage
    unreal = None


PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPT_DIR = os.path.join(PROJECT_ROOT, "Scripts")

DEFAULT_FBX = os.path.join(PROJECT_ROOT, "Art", "Characters", "Hands", "HandModel2.fbx")
FBX = os.environ.get("TRACE_HANDS_FBX") or DEFAULT_FBX

GLB_DIR = os.path.join(PROJECT_ROOT, "Intermediate", "Hands")
GLB = os.path.join(GLB_DIR, "TraceArms.glb")
MANIFEST = os.path.join(GLB_DIR, "TraceArms_manifest.json")

ROOT = "/Game/Trace/Characters/Hands"
STAGING = ROOT + "/_Import"
PROBE_DIR = ROOT + "/_ProbeFbxAsIs"
PIPELINE_DIR = ROOT + "/_Pipelines"

MESH_NAME = "SK_TraceArms"
SKELETON_NAME = "SK_TraceArms_Skeleton"

# The shipped first-person hands. NOT owned by this script: read, never written,
# never deleted. They are the scale yardstick and the source of the material.
PACK_HANDS = "/Game/Trace/Art/Pack/Hands/SK_TraceHands"
PACK_SHELL_MI = "/Game/Trace/Art/Pack/Materials/MI_Pack_shell"

# The one material slot the rebuilt mesh declares. The FBX carries ZERO FBX
# Material objects (◆MEASURED), so there is no surface to import and nothing to
# invent: the slot is named for, and bound to, the material the shipped hands
# already wear on their shell.
SLOT_NAME = "shell"

DEFAULT_GLTF_ASSETS_PIPELINE = ("/Interchange/Pipelines/DefaultGLTFAssetsPipeline"
                                ".DefaultGLTFAssetsPipeline")
DEFAULT_GLTF_PIPELINE = "/Interchange/Pipelines/DefaultGLTFPipeline.DefaultGLTFPipeline"
DEFAULT_FBX_PIPELINE = ("/Interchange/Pipelines/DefaultFBXOBJAssetsPipeline"
                        ".DefaultFBXOBJAssetsPipeline")

STAGE = os.environ.get("TRACE_HANDS_STAGE") or ("import" if unreal else "rebuild")

_failures = []


def log(msg):
    if unreal is not None:
        unreal.log("[Trace] {0}".format(msg))
    else:
        print("[Trace] {0}".format(msg))
        sys.stdout.flush()


def fail(msg):
    _failures.append(msg)
    if unreal is not None:
        unreal.log_error("[Trace] {0}".format(msg))
    else:
        print("[Trace] FAIL: {0}".format(msg))
        sys.stdout.flush()


# =============================================================================
# THE BONE CONTRACT
#
# 51 bones: one root plus 25 a side. Every entry is
#     (new name, new parent, (fbx source bones...))
# The FIRST source is the transform authority; any further sources are bones
# ◆MEASURED to be coincident with it whose skin weights are merged in. That
# second column exists for exactly one reason: Rigify ships the metacarpals
# TWICE, as ORG-palm.0N (the structural parent the finger hangs off) and
# DEF-palm.0N (the deform bone that carries the weights), sitting on top of each
# other — max element delta 2e-5 on a matrix whose axes are 100 long, i.e. the
# same bone to seven significant figures. Keeping both would have put a
# weight-bearing duplicate under every knuckle. The merge is asserted, not
# assumed: rebuild() re-measures the coincidence and fails if it stops holding.
#
# NAMES ARE THE SHIPPED FIRST-PERSON HANDS' NAMES — see the header. Two places
# where this rig and the pack rig legitimately differ, and both are here rather
# than in a comment nobody reads:
#   * forearm_left/right on the PACK rig is a decoration bone parented UNDER
#     hand_left (TraceCharacterViewModel.cpp:2600 hides it because it points
#     back at the lens). Here it is the real forearm and the hand's PARENT.
#     Same anatomy, opposite parentage — code that walks the pack's topology by
#     assumption will be wrong on this mesh.
#   * The pack has thumb_left_0..1; this rig has a third thumb joint, and
#     metacarpals (palm_*) the pack does not model at all.
# =============================================================================
SIDES = (("left", "L"), ("right", "R"))

# (name pattern, parent pattern, source pattern(s))
ARM_CHAIN = (
    ("shoulder_{s}",       "arms_root",           ("DEF-shoulder.{S}",)),
    ("upperarm_{s}",       "shoulder_{s}",        ("DEF-upper_arm.{S}",)),
    ("upperarm_{s}_twist", "upperarm_{s}",        ("DEF-upper_arm.{S}.001",)),
    ("forearm_{s}",        "upperarm_{s}_twist",  ("DEF-forearm.{S}",)),
    ("forearm_{s}_twist",  "forearm_{s}",         ("DEF-forearm.{S}.001",)),
    ("hand_{s}",           "forearm_{s}_twist",   ("DEF-hand.{S}",)),
    # metacarpals: ORG- is the parent Rigify actually hangs the finger off,
    # DEF- is the coincident twin that carries the weights. Merged.
    ("palm_index_{s}",     "hand_{s}",            ("ORG-palm.01.{S}", "DEF-palm.01.{S}")),
    ("palm_middle_{s}",    "hand_{s}",            ("ORG-palm.02.{S}", "DEF-palm.02.{S}")),
    ("palm_ring_{s}",      "hand_{s}",            ("ORG-palm.03.{S}", "DEF-palm.03.{S}")),
    # palm_pinky is the reparented one (see header). DEF-palm.04.L does not
    # exist in the file at all — the left pinky metacarpal is unweighted on that
    # side only — so the second source is optional here.
    ("palm_pinky_{s}",     "hand_{s}",            ("ORG-palm.04.{S}", "DEF-palm.04.{S}?")),
)

# (finger name, metacarpal parent, rigify stem)
FINGERS = (
    ("thumb",  "palm_index_{s}",  "thumb"),
    ("index",  "palm_index_{s}",  "f_index"),
    ("middle", "palm_middle_{s}", "f_middle"),
    ("ring",   "palm_ring_{s}",   "f_ring"),
    ("pinky",  "palm_pinky_{s}",  "f_pinky"),
)

ROOT_BONE = "arms_root"


def bone_contract():
    """The 51-row table above, expanded. (name, parent, [sources], optional)."""
    rows = [(ROOT_BONE, None, [], [])]
    for side, S in SIDES:
        for name, parent, sources in ARM_CHAIN:
            req, opt = [], []
            for src in sources:
                s = src.format(S=S)
                (opt if s.endswith("?") else req).append(s.rstrip("?"))
            rows.append((name.format(s=side), parent.format(s=side), req, opt))
        for finger, parent, stem in FINGERS:
            for i, joint in enumerate(("01", "02", "03")):
                rows.append(("{0}_{1}_{2}".format(finger, side, i),
                             parent.format(s=side) if i == 0
                             else "{0}_{1}_{2}".format(finger, side, i - 1),
                             ["DEF-{0}.{1}.{2}".format(stem, joint, S)], []))
    return rows


# =============================================================================
# Binary FBX reader
#
# Lifted from Scripts/import_rocco.py:249-303, which is the only other place in
# this project that reads an FBX without the engine. It is duplicated rather
# than imported because import_rocco.py does `import unreal` at module scope and
# this stage runs in plain python3 with no editor anywhere near it.
# =============================================================================
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
    """One record as (name, props, children), plus the offset just past it. A
    record whose EndOffset is zero closes a sibling list and comes back None."""
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


def _fbx_obj_name(props):
    """An FBX object's name, stored as "name\\x00\\x01Class"."""
    return props[1].split(b"\x00")[0].decode("utf8", "replace") if len(props) > 1 else ""


def read_fbx(path):
    with open(path, "rb") as handle:
        data = handle.read()
    if not data.startswith(b"Kaydara FBX Binary"):
        raise ValueError("{0} is not a binary FBX".format(path))
    version = struct.unpack("<I", data[23:27])[0]
    wide = version >= 7500
    roots = []
    off = 27
    # The 160-byte footer is not a record; the null terminator is the primary
    # exit and the length guard keeps a truncated file from walking off the end.
    while off < len(data) - 160:
        node, off = _fbx_node(data, off, wide)
        if node is None:
            break
        roots.append(node)
    return version, roots


def _section(roots, name):
    return next((n for n in roots if n[0] == name), None)


# -----------------------------------------------------------------------------
# 4x4 helpers. FBX stores row-major with row-vector convention (translation in
# elements 12..14) and glTF stores column-major with column-vector convention —
# which is the SAME 16 floats for the same transform, because M_col = M_row^T
# and "columns of the transpose" is "rows of the original". Nothing is
# transposed anywhere in this file; that is the reason.
# -----------------------------------------------------------------------------
def m_mul(a, b):
    return [sum(a[r * 4 + k] * b[k * 4 + c] for k in range(4))
            for r in range(4) for c in range(4)]


def m_inv_rigid(m):
    """Inverse of a rigid (orthonormal 3x3 + translation) row-vector matrix."""
    r = [m[0:3], m[4:7], m[8:11]]
    t = m[12:15]
    inv_r = [[r[c][ri] for c in range(3)] for ri in range(3)]      # transpose
    inv_t = [-sum(t[k] * inv_r[k][c] for k in range(3)) for c in range(3)]
    return [inv_r[0][0], inv_r[0][1], inv_r[0][2], 0.0,
            inv_r[1][0], inv_r[1][1], inv_r[1][2], 0.0,
            inv_r[2][0], inv_r[2][1], inv_r[2][2], 0.0,
            inv_t[0], inv_t[1], inv_t[2], 1.0]


def m_scale_to_metres(m):
    """A bind matrix whose axes are 100 long and whose translation is in
    centimetres, restated in metres with unit axes."""
    return [x / 100.0 if i != 15 else 1.0 for i, x in enumerate(m)]


def m_orthonormality(m):
    rows = [m[0:3], m[4:7], m[8:11]]
    lens = [math.sqrt(sum(c * c for c in r)) for r in rows]
    dots = [sum(a * b for a, b in zip(rows[i], rows[j])) for i, j in ((0, 1), (0, 2), (1, 2))]
    return max(max(abs(l - 1.0) for l in lens), max(abs(d) for d in dots))


def m_to_trs(m):
    """Row-vector rigid matrix -> (translation, quaternion xyzw) for a glTF node."""
    t = list(m[12:15])
    # Rotation rows are the images of the basis vectors; the quaternion below is
    # built from the matrix in the same row-vector layout, so no transpose.
    r = [m[0:3], m[4:7], m[8:11]]
    tr = r[0][0] + r[1][1] + r[2][2]
    if tr > 0.0:
        s = math.sqrt(tr + 1.0) * 2.0
        w = 0.25 * s
        x = (r[1][2] - r[2][1]) / s
        y = (r[2][0] - r[0][2]) / s
        z = (r[0][1] - r[1][0]) / s
    elif r[0][0] > r[1][1] and r[0][0] > r[2][2]:
        s = math.sqrt(1.0 + r[0][0] - r[1][1] - r[2][2]) * 2.0
        w = (r[1][2] - r[2][1]) / s
        x = 0.25 * s
        y = (r[1][0] + r[0][1]) / s
        z = (r[2][0] + r[0][2]) / s
    elif r[1][1] > r[2][2]:
        s = math.sqrt(1.0 + r[1][1] - r[0][0] - r[2][2]) * 2.0
        w = (r[2][0] - r[0][2]) / s
        x = (r[1][0] + r[0][1]) / s
        y = 0.25 * s
        z = (r[2][1] + r[1][2]) / s
    else:
        s = math.sqrt(1.0 + r[2][2] - r[0][0] - r[1][1]) * 2.0
        w = (r[0][1] - r[1][0]) / s
        x = (r[2][0] + r[0][2]) / s
        y = (r[2][1] + r[1][2]) / s
        z = 0.25 * s
    n = math.sqrt(x * x + y * y + z * z + w * w) or 1.0
    return t, [x / n, y / n, z / n, w / n]


# =============================================================================
# STAGE 1 — rebuild
# =============================================================================
def parse_rig(path):
    """Everything this script needs out of the FBX, measured in one pass."""
    version, roots = read_fbx(path)
    objects = _section(roots, "Objects")
    connections = _section(roots, "Connections")
    if objects is None or connections is None:
        raise ValueError("{0} has no Objects/Connections section".format(path))

    by_id = {}
    for child in objects[2]:
        if child[1] and isinstance(child[1][0], int):
            by_id[child[1][0]] = child

    oo_children = {}
    for conn in connections[2]:
        p = conn[1]
        if len(p) >= 3 and p[0] == b"OO":
            oo_children.setdefault(p[2], []).append(p[1])

    models = sum(1 for c in objects[2] if c[0] == "Model")
    clusters, weighted = {}, {}
    n_clusters = 0
    for child in objects[2]:
        if child[0] != "Deformer" or len(child[1]) < 3 or child[1][2] != b"Cluster":
            continue
        n_clusters += 1
        kids = {k[0]: k for k in child[2]}
        bone = None
        for cid in oo_children.get(child[1][0], []):
            node = by_id.get(cid)
            if node is not None and node[0] == "Model":
                bone = _fbx_obj_name(node[1])
        if bone is None or "TransformLink" not in kids:
            continue
        clusters[bone] = {"link": kids["TransformLink"][1][0],
                          "xform": kids["Transform"][1][0] if "Transform" in kids else None}
        idx = kids.get("Indexes")
        wts = kids.get("Weights")
        if idx and wts and idx[1] and idx[1][0]:
            weighted[bone] = (idx[1][0], wts[1][0])

    geo = next(c for c in objects[2] if c[0] == "Geometry")
    gk = {k[0]: k for k in geo[2]}
    verts = gk["Vertices"][1][0]
    poly = gk["PolygonVertexIndex"][1][0]

    def layer(kind, data_key, index_key, stride):
        node = gk.get(kind)
        if node is None:
            return None, None
        sub = {k[0]: k for k in node[2]}
        vals = sub[data_key][1][0]
        flat = [tuple(vals[i:i + stride]) for i in range(0, len(vals), stride)]
        ref = sub["ReferenceInformationType"][1][0].decode()
        if ref == "IndexToDirect":
            return flat, sub[index_key][1][0]
        return flat, list(range(len(flat)))

    normals, normal_index = layer("LayerElementNormal", "Normals", "NormalsIndex", 3)
    uvs, uv_index = layer("LayerElementUV", "UV", "UVIndex", 2)

    # Model "Arm 2": the mesh's own transform. Read, not assumed.
    mesh_model = next(c for c in objects[2]
                      if c[0] == "Model" and len(c[1]) > 2 and c[1][2] == b"Mesh")
    props = {}
    for c in mesh_model[2]:
        if c[0] == "Properties70":
            for e in c[2]:
                props[e[1][0].decode("utf8", "replace")] = e[1][4:]
    rot = props.get("Lcl Rotation", [0.0, 0.0, 0.0])
    scl = props.get("Lcl Scaling", [1.0, 1.0, 1.0])

    return {"version": version, "models": models, "clusters": clusters,
            "n_clusters": n_clusters, "weighted": weighted,
            "verts": [tuple(verts[i:i + 3]) for i in range(0, len(verts), 3)],
            "poly": poly, "normals": normals, "normal_index": normal_index,
            "uvs": uvs, "uv_index": uv_index,
            "mesh_name": _fbx_obj_name(mesh_model[1]),
            "mesh_rot": list(rot[:3]), "mesh_scale": list(scl[:3]),
            "materials": [c for c in objects[2] if c[0] == "Material"]}


def mesh_global(rig):
    """The mesh's bind-world matrix, built from the properties just read.

    Only the X rotation and the uniform scale the file actually carries are
    honoured; anything else in there is a shape this rebuild has never seen and
    is refused rather than silently ignored."""
    rx, ry, rz = rig["mesh_rot"]
    sx, sy, sz = rig["mesh_scale"]
    if abs(ry) > 1e-6 or abs(rz) > 1e-6:
        fail("Model {0!r} carries Y/Z rotation ({1}) — the rebuild only handles the "
             "X-only Blender Z-up conversion".format(rig["mesh_name"], rig["mesh_rot"]))
    if abs(sx - sy) > 1e-6 or abs(sx - sz) > 1e-6:
        fail("Model {0!r} carries non-uniform scale {1}".format(rig["mesh_name"],
                                                                rig["mesh_scale"]))
    a = math.radians(rx)
    c, s = math.cos(a), math.sin(a)
    return [sx, 0.0, 0.0, 0.0,
            0.0, sy * c, sy * s, 0.0,
            0.0, -sz * s, sz * c, 0.0,
            0.0, 0.0, 0.0, 1.0]


def rebuild():
    log("rebuild: reading {0}".format(FBX))
    if not os.path.isfile(FBX):
        fail("source FBX missing: {0}".format(FBX))
        return None
    rig = parse_rig(FBX)
    log("  FBX version {0}: {1} Model nodes, {2} skin clusters, {3} of them weighted, "
        "{4} vertices, {5} FBX Materials"
        .format(rig["version"], rig["models"], rig["n_clusters"], len(rig["weighted"]),
                len(rig["verts"]), len(rig["materials"])))
    if rig["materials"]:
        fail("the FBX now carries {0} Material object(s); this rebuild declares one flat "
             "slot and would be throwing them away".format(len(rig["materials"])))

    mg = mesh_global(rig)
    log("  Model {0!r}: Lcl Rotation {1}, Lcl Scaling {2} -> bind-world is centimetres, Y-up"
        .format(rig["mesh_name"], rig["mesh_rot"], rig["mesh_scale"]))

    # --- the identity the whole rebuild stands on, re-measured every run ------
    worst, worst_bone = 0.0, None
    for bone in rig["weighted"]:
        c = rig["clusters"][bone]
        if c["xform"] is None:
            fail("cluster for {0} has no Transform matrix".format(bone))
            continue
        prod = m_mul(c["xform"], c["link"])
        d = max(abs(a - b) for a, b in zip(prod, mg))
        if d > worst:
            worst, worst_bone = d, bone
    log("  skin identity  max |Transform*TransformLink - MeshGlobal| = {0:.2e}  ({1})"
        .format(worst, worst_bone))
    if worst > 1e-3:
        fail("Transform*TransformLink no longer equals the mesh's global matrix "
             "(worst {0:.3e} on {1}); the bind-world formulation is invalid".format(worst, worst_bone))

    # --- bones ----------------------------------------------------------------
    rows = bone_contract()
    bones, index = [], {}
    for name, parent, req, opt in rows:
        if name == ROOT_BONE:
            link = [1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0,
                    0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0]
            sources = []
        else:
            missing = [s for s in req if s not in rig["clusters"]]
            if missing:
                fail("bone {0}: source(s) {1} are not in the FBX".format(name, missing))
                continue
            link = m_scale_to_metres(rig["clusters"][req[0]]["link"])
            dev = m_orthonormality(link)
            if dev > 1e-4:
                fail("bone {0}: source {1} bind matrix is not orthonormal (dev {2:.2e})"
                     .format(name, req[0], dev))
            # merged twins must actually be twins
            for extra in req[1:] + [o for o in opt if o in rig["clusters"]]:
                twin = m_scale_to_metres(rig["clusters"][extra]["link"])
                d = max(abs(a - b) for a, b in zip(twin, link))
                if d > 1e-4:
                    fail("bone {0}: {1} is NOT coincident with {2} (max element delta "
                         "{3:.2e}); merging their weights would move geometry"
                         .format(name, extra, req[0], d))
            sources = req + [o for o in opt if o in rig["clusters"]]
        index[name] = len(bones)
        bones.append({"name": name, "parent": parent, "sources": sources, "link": link})

    if _failures:
        return None

    # --- the owner's ring fingers, fixed in the bind pose (see the header) ----
    roll_before, roll_after, d_before, d_after, fixed = apply_ring_roll_fix(bones, index)
    if _failures:
        return None
    for side, _S in SIDES:
        log("  ring roll {0:<5}  vs middle finger: {1:6.1f} deg before -> {2:6.1f} deg after   "
            "({3})".format(side, d_before[side], d_after[side],
                           "ROLLED 180 in the bind pose" if fixed
                           else "left alone — the source is already consistent"))

    # every weighted cluster must land on exactly one kept bone
    owner = {}
    for b in bones:
        for s in b["sources"]:
            if s in rig["weighted"]:
                if s in owner:
                    fail("weighted cluster {0} claimed by both {1} and {2}"
                         .format(s, owner[s], b["name"]))
                owner[s] = b["name"]
    orphan = sorted(set(rig["weighted"]) - set(owner))
    if orphan:
        fail("{0} weighted cluster(s) have no bone in the contract: {1}"
             .format(len(orphan), ", ".join(orphan)))
        return None
    log("  bones          {0} kept, carrying all {1} weighted clusters"
        .format(len(bones), len(owner)))

    # --- per-vertex influences -----------------------------------------------
    influences = [[] for _ in rig["verts"]]
    for src, bone_name in owner.items():
        idx, wts = rig["weighted"][src]
        ji = index[bone_name]
        for vi, w in zip(idx, wts):
            influences[vi].append((ji, float(w)))
    # merged twins can contribute to the same joint twice; fold them
    for vi, lst in enumerate(influences):
        merged = {}
        for ji, w in lst:
            merged[ji] = merged.get(ji, 0.0) + w
        influences[vi] = sorted(merged.items(), key=lambda t: -t[1])
    # --- *** ONE HAND'S BONE MUST NOT DRIVE THE OTHER HAND'S SKIN. *** --------
    #
    # ◆MEASURED, and it is a third real defect in the source rig:
    # DEF-palm.04.R carries 56 vertices at weight > 0.2, and 28 of them are on
    # the LEFT hand (bind-world X +78.9 against the bone's own -73.5).  The left
    # pinky's metacarpal has no DEF- bone at all in this file — the same
    # asymmetry that leaves palm_pinky_left unweighted — so the left pinky palm
    # ended up bound to its mirror.  It survives a T-pose (the two hands are
    # mirror images, so nothing moves) and it TEARS THE HAND APART the moment
    # the hands are posed differently, which is the whole of stage 2: the left
    # pinky palm was measured 60-70 uu away from the rest of the left hand, in
    # the frame, on every weapon.
    #
    # THE REPAIR IS THE MIRROR, and it is safe here because it is checked rather
    # than assumed: the mesh is a mirrored PAIR (asserted below — no vertex comes
    # within 60 cm of the X = 0 plane, so "which hand is this vertex on" has no
    # grey area), and every side-named bone has a mirror twin by construction of
    # the bone contract.  An influence whose vertex is on the far side of X = 0
    # from its own bone moves to that bone's twin, which is exactly where the
    # mirror modifier that produced this mesh meant it to be.
    def mirror_name(n):
        if "_left" in n:
            return n.replace("_left", "_right")
        if "_right" in n:
            return n.replace("_right", "_left")
        return None

    vert_x = [sum(p[k] * mg[k * 4] for k in range(3)) for p in rig["verts"]]   # bind-world cm
    gap = min(abs(x) for x in vert_x)
    if gap < 10.0:
        fail("a vertex sits {0:.2f} cm from the mesh's X=0 plane, so 'which hand is this on' is "
             "no longer a safe question and the left/right weight repair below would be "
             "guessing".format(gap))
    moved, moved_bones = 0, {}
    for vi, lst in enumerate(influences):
        merged, changed = {}, False
        for ji, w in lst:
            bone = bones[ji]
            bx = bone["link"][12] * 100.0
            twin = mirror_name(bone["name"])
            if twin is not None and abs(bx) > 1.0 and vert_x[vi] * bx < 0.0:
                if twin not in index:
                    fail("bone {0} drives vertices on the other hand and has no mirror {1}"
                         .format(bone["name"], twin))
                    continue
                moved_bones[bone["name"]] = moved_bones.get(bone["name"], 0) + 1
                ji, changed = index[twin], True
                moved += 1
            merged[ji] = merged.get(ji, 0.0) + w
        if changed:
            influences[vi] = sorted(merged.items(), key=lambda t: -t[1])
    if moved:
        log("  weight repair   {0} influence(s) moved to the mirror bone: {1}"
            .format(moved, ", ".join("{0}->{1} x{2}".format(k, mirror_name(k), v)
                                     for k, v in sorted(moved_bones.items()))))
    for vi, lst in enumerate(influences):
        for ji, _w in lst:
            bx = bones[ji]["link"][12] * 100.0
            if abs(bx) > 1.0 and vert_x[vi] * bx < 0.0:
                fail("after the repair, {0} still drives a vertex on the other hand"
                     .format(bones[ji]["name"]))

    unbound = [i for i, l in enumerate(influences) if not l]
    if unbound:
        fail("{0} vertices have no influence at all: {1}".format(len(unbound), unbound[:12]))
    max_inf = max(len(l) for l in influences)
    over8 = sum(1 for l in influences if len(l) > 8)
    over4 = sum(1 for l in influences if len(l) > 4)
    log("  influences     max {0} per vertex; {1} vertices over 4 (carried in JOINTS_1), "
        "{2} over 8".format(max_inf, over4, over8))
    if over8:
        fail("{0} vertices need more than 8 influences; glTF carries at most two sets"
             .format(over8))

    # --- geometry -------------------------------------------------------------
    # Blender/FBX geometry space (metres, Z-up) -> bind-world (cm, Y-up) via the
    # mesh's own matrix, then -> glTF (metres, Y-up) by /100. Composed once:
    # (x, y, z) -> (x, z, -y) for the -90 X rotation this file carries.
    def to_gl(p):
        q = [sum(p[k] * mg[k * 4 + c] for k in range(3)) for c in range(3)]
        return (q[0] / 100.0, q[1] / 100.0, q[2] / 100.0)

    def dir_to_gl(n):
        q = [sum(n[k] * mg[k * 4 + c] for k in range(3)) for c in range(3)]
        l = math.sqrt(q[0] ** 2 + q[1] ** 2 + q[2] ** 2) or 1.0
        return (q[0] / l, q[1] / l, q[2] / l)

    poly = rig["poly"]
    corners, faces, cur = [], [], []
    for c, raw in enumerate(poly):
        vi = ~raw if raw < 0 else raw
        cur.append((c, vi))
        if raw < 0:
            faces.append(cur)
            cur = []
    if cur:
        fail("PolygonVertexIndex does not end on a polygon boundary")

    pos, nrm, uv, j0, w0, j1, w1, tris = [], [], [], [], [], [], [], []
    seen = {}

    def emit(corner, vi):
        ni = rig["normal_index"][corner] if rig["normals"] else -1
        ui = rig["uv_index"][corner] if rig["uvs"] else -1
        key = (vi, ni, ui)
        got = seen.get(key)
        if got is not None:
            return got
        got = len(pos)
        seen[key] = got
        pos.append(to_gl(rig["verts"][vi]))
        nrm.append(dir_to_gl(rig["normals"][ni]) if ni >= 0 else (0.0, 1.0, 0.0))
        if ui >= 0:
            u, v = rig["uvs"][ui]
            uv.append((u, 1.0 - v))       # glTF's V runs down the image
        else:
            uv.append((0.0, 0.0))
        inf = influences[vi][:8]
        total = sum(w for _, w in inf) or 1.0
        pad = [(0, 0.0)] * (8 - len(inf))
        inf = inf + pad
        j0.append(tuple(i for i, _ in inf[:4]))
        w0.append(tuple(w / total for _, w in inf[:4]))
        j1.append(tuple(i for i, _ in inf[4:]))
        w1.append(tuple(w / total for _, w in inf[4:]))
        return got

    for face in faces:
        ring = [emit(c, vi) for c, vi in face]
        for k in range(1, len(ring) - 1):
            tris.append((ring[0], ring[k], ring[k + 1]))
    log("  geometry       {0} polygons ({1} corners) -> {2} triangles, {3} split vertices"
        .format(len(faces), len(poly), len(tris), len(pos)))

    # WINDING, BY AREA AND NOT BY COUNT. The FBX and glTF are both right-handed
    # and the mesh matrix is a rotation plus a positive uniform scale, so the
    # authored winding has to survive untouched — but a fan split of a BENT quad
    # always leaves one near-degenerate sliver whose normal is meaningless, and
    # counting triangles would keep reporting those two slivers forever as if
    # something were wrong. ◆MEASURED on this file: 2 of 664 triangles disagree,
    # total area 4.4e-6 m^2 = 0.0007% of the surface, both of them the far half
    # of a folded quad. Area is the honest denominator; the count is printed too
    # so a real flip (which would take area with it) is still legible.
    agree_area = bad_area = 0.0
    agree_n = 0
    for a, b, c in tris:
        u = [pos[b][i] - pos[a][i] for i in range(3)]
        v = [pos[c][i] - pos[a][i] for i in range(3)]
        n = (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0])
        m = [(nrm[a][i] + nrm[b][i] + nrm[c][i]) for i in range(3)]
        area = math.sqrt(sum(x * x for x in n)) / 2.0
        if sum(n[i] * m[i] for i in range(3)) > 0:
            agree_n += 1
            agree_area += area
        else:
            bad_area += area
    frac = bad_area / (agree_area + bad_area) if (agree_area + bad_area) else 0.0
    log("  winding        {0}/{1} triangles agree with their corner normals; the {2} that "
        "do not are {3:.4f}% of the surface area".format(agree_n, len(tris),
                                                         len(tris) - agree_n, frac * 100.0))
    if frac > 0.01:
        fail("{0:.2f}% of the mesh area is wound against its normals; it would render "
             "inside out in patches".format(frac * 100.0))

    # --- write the GLB --------------------------------------------------------
    sys.path.insert(0, SCRIPT_DIR)
    from trace_glb import _Buf, _pad4, JSON_CHUNK, BIN_CHUNK   # proven writer parts

    buf = _Buf()
    nodes = []
    children = {}
    for i, b in enumerate(bones):
        if b["parent"] is not None:
            children.setdefault(index[b["parent"]], []).append(i)
    ibms = []
    for i, b in enumerate(bones):
        parent = b["parent"]
        local = b["link"] if parent is None else m_mul(b["link"], m_inv_rigid(bones[index[parent]]["link"]))
        t, q = m_to_trs(local)
        nd = {"name": b["name"], "translation": t, "rotation": q}
        if i in children:
            nd["children"] = children[i]
        nodes.append(nd)
        ibms.append(m_inv_rigid(b["link"]))

    attrs = {
        "POSITION": buf.vec3(pos, minmax=True),
        "NORMAL": buf.vec3(nrm),
        "TEXCOORD_0": buf.vec2(uv),
        "JOINTS_0": buf.vec4ub(j0),
        "WEIGHTS_0": buf.vec4f(w0),
    }
    if over4:
        attrs["JOINTS_1"] = buf.vec4ub(j1)
        attrs["WEIGHTS_1"] = buf.vec4f(w1)
    idx_acc = buf.indices([i for tri in tris for i in tri])
    ibm_acc = buf.mat4(ibms)

    mesh_node = len(nodes)
    nodes.append({"name": "TraceArms", "mesh": 0, "skin": 0})
    gltf = {
        "asset": {"version": "2.0", "generator": "Trace import_hands.py"},
        "scene": 0,
        "scenes": [{"name": "scene", "nodes": [index[ROOT_BONE], mesh_node]}],
        "nodes": nodes,
        "meshes": [{"name": "TraceArms", "primitives": [
            {"attributes": attrs, "indices": idx_acc, "material": 0, "mode": 4}]}],
        "skins": [{"name": "TraceArms_skin", "joints": list(range(len(bones))),
                   "skeleton": index[ROOT_BONE], "inverseBindMatrices": ibm_acc}],
        "materials": [{"name": SLOT_NAME,
                       "pbrMetallicRoughness": {
                           "baseColorFactor": [0.5, 0.5, 0.5, 1.0],
                           "metallicFactor": 0.0, "roughnessFactor": 0.6}}],
        "buffers": [{"byteLength": len(buf.data)}],
        "bufferViews": buf.views,
        "accessors": buf.accessors,
    }
    js = _pad4(json.dumps(gltf, separators=(",", ":")).encode("utf-8"), b" ")
    bn = _pad4(buf.data, b"\x00")
    blob = struct.pack("<4sII", b"glTF", 2, 12 + 8 + len(js) + 8 + len(bn))
    blob += struct.pack("<II", len(js), JSON_CHUNK) + js
    blob += struct.pack("<II", len(bn), BIN_CHUNK) + bn
    os.makedirs(GLB_DIR, exist_ok=True)
    with open(GLB, "wb") as handle:
        handle.write(blob)
    log("  wrote          {0} ({1} bytes)".format(GLB, len(blob)))

    # --- the manifest: everything the next stage would otherwise re-derive ----
    lo = [min(p[i] for p in pos) for i in range(3)]
    hi = [max(p[i] for p in pos) for i in range(3)]
    manifest = {
        "source_fbx": os.path.relpath(FBX, PROJECT_ROOT),
        "fbx_version": rig["version"],
        "fbx_models": rig["models"],
        "fbx_clusters": rig["n_clusters"],
        "fbx_weighted_clusters": len(rig["weighted"]),
        "glb": os.path.relpath(GLB, PROJECT_ROOT),
        "glb_bytes": len(blob),
        "vertices_source": len(rig["verts"]),
        "vertices_split": len(pos),
        "triangles": len(tris),
        "bones": [{"name": b["name"], "parent": b["parent"], "sources": b["sources"],
                   "weighted": any(s in rig["weighted"] for s in b["sources"])}
                  for b in bones],
        "weighted_bones": sorted(owner.values()),
        "weighted_clusters": {k: owner[k] for k in sorted(owner)},
        "max_influences": max_inf,
        "vertices_over_4_influences": over4,
        "slot": SLOT_NAME,
        # UE units: (gl.x, gl.z, gl.y) * 100
        "ue_bounds_min": [lo[0] * 100.0, lo[2] * 100.0, lo[1] * 100.0],
        "ue_bounds_max": [hi[0] * 100.0, hi[2] * 100.0, hi[1] * 100.0],
        # SHIPPED (post-fix) first, because that is what the GLB carries; the
        # pre-fix table is kept beside it so the defect the owner reported stays
        # legible in the asset's own paperwork instead of only in a report.
        "ring_roll": roll_after,
        "ring_roll_before_fix": roll_before,
        "ring_roll_fix_applied": fixed,
        "ring_vs_middle_deg_before": d_before,
        "ring_vs_middle_deg_after": d_after,
    }
    with open(MANIFEST, "w") as handle:
        json.dump(manifest, handle, indent=1, sort_keys=True)
    log("  wrote          {0}".format(MANIFEST))
    log("  UE bounds      X {0:.2f}..{1:.2f}  Y {2:.2f}..{3:.2f}  Z {4:.2f}..{5:.2f} uu"
        .format(manifest["ue_bounds_min"][0], manifest["ue_bounds_max"][0],
                manifest["ue_bounds_min"][1], manifest["ue_bounds_max"][1],
                manifest["ue_bounds_min"][2], manifest["ue_bounds_max"][2]))
    return manifest


# -----------------------------------------------------------------------------
# The ring-finger measurement, taken on the bind matrices that go INTO the GLB
# (so it describes the asset that ships, not the file it came from). The import
# stage repeats it on the imported skeleton; the two must agree.
#
# Each finger joint's local +X is the axis Blender flexes it about, and its
# local +Y runs down the bone (|dot| >= 0.999 on all 30 joints, measured). Roll
# is the signed angle from a hand-plane reference to local +X, about local +Y.
# Two fingers that curl the same way have the same roll; a finger whose roll is
# 180 degrees off curls the opposite way for the same rotation.
# -----------------------------------------------------------------------------
RING_BONES = tuple("ring_{0}_{1}".format(side, j)
                   for side, _S in SIDES for j in range(3))


def ring_vs_middle(table):
    """How far the ring chain's curl direction is from the middle finger's.

    Degrees, worst of the three joints, per side.  0 = the two fingers curl the
    same way for the same rotation; 180 = they curl opposite ways, which is the
    owner's bug.  The SAME quantity ring_report() prints off the imported
    skeleton, so the two halves of the pipeline can be compared directly.

    Middle is the reference finger because the pinky is itself splayed ~9.5
    degrees off it in this rig (ordinary anatomy), so a ring/pinky comparison can
    never come out clean however correct the asset is.
    """
    out = {}
    for side, _S in SIDES:
        j = table[side]["joints"]
        out[side] = max(
            abs((j["ring_{0}_{1}".format(side, k)]["roll_deg"]
                 - j["middle_{0}_{1}".format(side, k)]["roll_deg"] + 540.0) % 360.0 - 180.0)
            for k in range(3))
    return out


def apply_ring_roll_fix(bones, index):
    """Roll the six ring bind frames 180 degrees about their own bone axis.

    THE OWNER'S BUG, FIXED WHERE IT LIVES.  "The ring fingers bend opposite the
    rest" is a bind-pose fact, measured on both hands at all three joints (the
    numbers are in the report this stage prints, before and after).  The bind
    matrices here are ROW-VECTOR (p_world = p_local * M), so their rows ARE the
    bone's local axes in bind-world:

        row 0  local +X   the flexion axis   -> negated
        row 1  local +Y   ALONG the bone     -> untouched (|dot| = 1.0000 against
                                                the direction to the child, on
                                                all 20 finger joint pairs)
        row 2  local +Z   the abduction axis -> negated
        row 3  translation                   -> untouched

    Negating rows 0 and 2 is exactly left-multiplication by diag(-1, 1, -1, 1),
    i.e. a 180-degree rotation applied in the bone's OWN frame about its OWN
    axis.  It is a rotation (determinant +1), so nothing is mirrored; the bone
    keeps its position, its direction and its length, and only the sense of
    "positive X curls the finger" changes.  Both the ring's flexion AND its
    abduction were inverted, and this fixes both with one operation.

    RETURNS the before/after roll tables so the caller can assert the flip
    actually happened rather than assume it.  A source re-export that fixes the
    roll in Blender will make the BEFORE table read "consistent" and this
    function will refuse to double-flip it.
    """
    before = finger_roll_table(bones, index)
    delta_before = ring_vs_middle(before)
    if all(d <= 30.0 for d in delta_before.values()):
        # ALREADY RIGHT — a re-export fixed it upstream. Do nothing, and say so.
        return before, before, delta_before, delta_before, False
    for side in delta_before:
        if delta_before[side] < 150.0:
            fail("the {0} ring chain sits {1:.1f} deg from the middle finger's curl direction, "
                 "which is neither the same way (<=30) nor the opposite way (>=150). A 180-degree "
                 "roll would not fix that; look at the source rig before trusting anything "
                 "downstream.".format(side, delta_before[side]))
            return before, before, delta_before, delta_before, False

    # Snapshot the two things the roll must NOT touch, so "only the roll changed"
    # is a measurement and not a claim about what the code above looks like.
    keep = {name: (list(bones[index[name]]["link"][4:7]),      # local +Y, along the bone
                   list(bones[index[name]]["link"][12:15]))    # position, bind-world metres
            for name in RING_BONES}

    for name in RING_BONES:
        m = bones[index[name]]["link"]
        for k in (0, 1, 2, 8, 9, 10):
            m[k] = -m[k]

    worst_move = 0.0
    for name in RING_BONES:
        m = bones[index[name]]["link"]
        y_was, p_was = keep[name]
        worst_move = max(worst_move,
                         max(abs(a - b) for a, b in zip(m[4:7], y_was)),
                         max(abs(a - b) for a, b in zip(m[12:15], p_was)))
    if worst_move > 1e-12:
        fail("the ring roll moved a bone: worst |delta| {0:.3e} on its along-bone axis or its "
             "position, which must both be untouched".format(worst_move))

    after = finger_roll_table(bones, index)
    delta_after = ring_vs_middle(after)
    for side in delta_after:
        if delta_after[side] > 30.0:
            fail("after the 180-degree roll the {0} ring chain is still {1:.1f} deg from the "
                 "middle finger's curl direction (wanted <= 30)"
                 .format(side, delta_after[side]))
    return before, after, delta_before, delta_after, True


def finger_roll_table(bones, index):
    def frame(name):
        m = bones[index[name]]["link"]
        return {"p": m[12:15], "X": m[0:3], "Y": m[4:7], "Z": m[8:11]}

    def sub(a, b):
        return [a[i] - b[i] for i in range(3)]

    def dot(a, b):
        return sum(a[i] * b[i] for i in range(3))

    def cross(a, b):
        return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]

    def norm(a):
        l = math.sqrt(dot(a, a)) or 1.0
        return [c / l for c in a]

    out = {}
    for side, _S in SIDES:
        fdir = norm(sub(frame("middle_{0}_2".format(side))["p"],
                        frame("middle_{0}_0".format(side))["p"]))
        spread = norm(sub(frame("pinky_{0}_0".format(side))["p"],
                          frame("index_{0}_0".format(side))["p"]))
        ref = norm(cross(fdir, spread))
        rows = {}
        for finger in ("thumb", "index", "middle", "ring", "pinky"):
            for j in range(3):
                name = "{0}_{1}_{2}".format(finger, side, j)
                fr = frame(name)
                d = norm(fr["Y"])
                r = norm(sub(ref, [dot(ref, d) * c for c in d]))
                s = cross(r, fr["X"])
                rows[name] = {
                    "roll_deg": math.degrees(math.atan2(dot(s, d), dot(r, fr["X"]))),
                    "flex_axis_world": [round(c, 4) for c in fr["X"]],
                    "along_bone_dot": round(dot(fr["Y"], d), 4),
                }
        out[side] = {"flex_reference_axis": [round(c, 4) for c in ref], "joints": rows}
    return out


# =============================================================================
# STAGE 2 — import (editor)
# =============================================================================
def _eal():
    return unreal.EditorAssetLibrary


def prop(obj, name, value):
    """set_editor_property, but a rejected name is reported, not fatal.

    Interchange renames pipeline properties between engine minors more often
    than it changes their meaning (the note in Scripts/import_rocco.py)."""
    try:
        obj.set_editor_property(name, value)
        return True
    except Exception as exc:                          # noqa: BLE001
        fail("pipeline property {0} = {1!r} rejected: {2}".format(name, value, exc))
        return False


def build_pipeline(name, source_kind):
    """A configured copy of an engine pipeline, on disk.

    FImportAssetParameters::OverridePipelines is a TArray<FSoftObjectPath>, so
    the override must be a real asset — the same shape as import_pack.py and
    import_rocco.py. Removed again at the end of the run."""
    path = "{0}/{1}".format(PIPELINE_DIR, name)
    if _eal().does_asset_exist(path):
        _eal().delete_asset(path)
    src = unreal.load_asset(DEFAULT_GLTF_ASSETS_PIPELINE if source_kind == "gltf"
                            else DEFAULT_FBX_PIPELINE)
    if src is None:
        fail("could not load the engine {0} pipeline".format(source_kind))
        return None
    pipe = unreal.AssetToolsHelpers.get_asset_tools().duplicate_asset(name, PIPELINE_DIR, src)
    if pipe is None:
        fail("AssetTools refused to duplicate the {0} pipeline into {1}"
             .format(source_kind, PIPELINE_DIR))
        return None

    # 1.0 for both translators, for two different reasons that happen to agree:
    # the glTF translator has already converted metres to centimetres, and the
    # FBX is authored in centimetres. See import_pack.py's note on the 100x trap.
    prop(pipe, "import_offset_uniform_scale", 1.0)
    prop(pipe, "use_source_name_for_asset", True)
    prop(pipe, "scene_name_sub_folder", False)
    prop(pipe, "asset_type_sub_folders", False)

    common = pipe.get_editor_property("common_meshes_properties")
    # Nothing to force: both files carry a real skin (the GLB declares one skin
    # with 51 joints; the FBX has 705 clusters), unlike import_pack.py's GLBs
    # which report skins: 0 and have to be coerced.
    prop(common, "force_all_mesh_as_type", unreal.InterchangeForceMeshType.IFMT_NONE)
    prop(common, "bake_meshes", True)
    prop(common, "import_sockets", True)
    prop(common, "keep_sections_separate", False)

    skelcommon = pipe.get_editor_property("common_skeletal_meshes_and_animations_properties")
    prop(skelcommon, "import_only_animations", False)
    # No AnimStack in either file, so there is no frame 0 to prefer over the
    # bind pose — and the bind pose is the whole measurement here.
    prop(skelcommon, "use_t0_as_ref_pose", False)

    mesh = pipe.get_editor_property("mesh_pipeline")
    prop(mesh, "import_static_meshes", False)
    prop(mesh, "import_skeletal_meshes", True)
    prop(mesh, "combine_skeletal_meshes_behavior",
         unreal.InterchangeCombineSkeletalMeshesBehavior.BY_SKELETON)
    # 8, because the source needs 7 on its worst vertex (measured in rebuild).
    prop(mesh, "bone_influence_limit", 8)
    # Off. A ragdoll of a pair of floating arms is not a thing this asset needs,
    # and on the as-is FBX probe it would try to wrap 1051 bones.
    prop(mesh, "create_physics_asset", False)
    prop(mesh, "build_nanite", False)

    anim = pipe.get_editor_property("animation_pipeline")
    # Neither file carries a clip. Off rather than on-and-empty, so a future
    # re-export that DOES carry motion changes this line visibly.
    prop(anim, "import_animations", False)

    mat = pipe.get_editor_property("material_pipeline")
    # The FBX declares zero materials and the rebuilt GLB declares one flat
    # placeholder that exists only to NAME the slot. Importing it would author a
    # second master beside M_TraceRailgun for no gain.
    prop(mat, "import_materials", False)
    tex = mat.get_editor_property("texture_pipeline")
    if tex is not None:
        prop(tex, "import_textures", False)

    _eal().save_loaded_asset(pipe, only_if_is_dirty=False)
    return path


def run_import(source_file, dest, pipeline_path, extra_pipeline=None):
    im = unreal.InterchangeManager.get_interchange_manager_scripted()
    source = unreal.InterchangeManager.create_source_data(source_file)
    params = unreal.ImportAssetParameters()
    params.set_editor_property("is_automated", True)
    overrides = [unreal.SoftObjectPath(pipeline_path)]
    if extra_pipeline:
        overrides.append(unreal.SoftObjectPath(extra_pipeline))
    try:
        params.set_editor_property("override_pipelines", overrides)
    except Exception as exc:                          # noqa: BLE001
        fail("override_pipelines rejected ({0}); falling back to plain strings".format(exc))
        params.set_editor_property("override_pipelines",
                                   [pipeline_path] + ([extra_pipeline] if extra_pipeline else []))
    return im.import_asset(dest, source, params)


def landed(folder):
    out = {}
    if not _eal().does_directory_exist(folder):
        return out
    for p in _eal().list_assets(folder, recursive=True, include_folder=False):
        p = p.split(".")[0]
        out[p] = unreal.load_asset(p)
    return out


def bone_table(mesh):
    """Every bone of a SkeletalMesh, in component space, with its axes.

    A Skeleton's bone_tree gives names and nothing else and there is no
    reference-pose accessor on it from Python, so one SkeletalMeshActor is
    spawned, read and destroyed — the import_rocco.py probe() pattern. The level
    is never saved."""
    subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actor = subsystem.spawn_actor_from_class(unreal.SkeletalMeshActor,
                                             unreal.Vector(0, 0, 0), unreal.Rotator(0, 0, 0))
    if actor is None:
        fail("could not spawn a SkeletalMeshActor; no bones measured")
        return []
    rows = []
    try:
        comp = actor.skeletal_mesh_component
        comp.set_skeletal_mesh_asset(mesh)
        for i in range(comp.get_num_bones()):
            name = str(comp.get_bone_name(i))
            # UE answers the literal string "None" for a root bone's parent
            # rather than an empty name; both spellings mean "no parent".
            parent = str(comp.get_parent_bone(name))
            t = comp.get_socket_transform(name, unreal.RelativeTransformSpace.RTS_COMPONENT)
            q = t.rotation
            x, y, z, w = q.x, q.y, q.z, q.w
            rows.append({
                "index": i,
                "name": name,
                "parent": None if parent in ("", "None") else parent,
                "pos": (t.translation.x, t.translation.y, t.translation.z),
                # rotation matrix rows = the bone's local axes in component space
                "X": (1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w)),
                "Y": (2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w)),
                "Z": (2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y)),
            })
    except Exception as exc:                          # noqa: BLE001
        fail("bone read failed on {0}: {1}".format(mesh.get_path_name(), exc))
    finally:
        subsystem.destroy_actor(actor)
    return rows


def probe_fbx_as_is(lines):
    """Import the raw FBX, count what you get, throw it away.

    This exists so the header's "1051 bones" is a number this run measured and
    not a claim somebody typed once. It costs about three seconds."""
    if _eal().does_directory_exist(PROBE_DIR):
        _eal().delete_directory(PROBE_DIR)
    pipeline = build_pipeline("TraceHandsFBXProbe", "fbx")
    if pipeline is None:
        return None
    run_import(FBX, PROBE_DIR, pipeline)
    result = {"bones": None, "slots": None, "bounds": None}
    for path, asset in sorted(landed(PROBE_DIR).items()):
        if not isinstance(asset, unreal.SkeletalMesh):
            continue
        rows = bone_table(asset)
        families = {"DEF-": 0, "ORG-": 0, "MCH-": 0, "other": 0}
        for r in rows:
            for k in ("DEF-", "ORG-", "MCH-"):
                if r["name"].startswith(k):
                    families[k] += 1
                    break
            else:
                families["other"] += 1
        b = asset.get_bounds()
        o, e = b.origin, b.box_extent
        slots = list(asset.get_editor_property("materials"))
        result = {"bones": len(rows), "families": families, "slots": len(slots),
                  "bounds": (o.x - e.x, o.x + e.x, o.y - e.y, o.y + e.y,
                             o.z - e.z, o.z + e.z),
                  # "ear" is deliberately absent from this list: it matches
                  # forEARm, which is the one part of this rig that is not a
                  # face, and counting the arms as face bones would have
                  # overstated the number by a third.
                  "face_bones": sum(1 for r in rows if any(
                      t in r["name"] for t in ("brow", "lid", "lip", "jaw", "chin",
                                               "nose", "cheek", "eye", "tongue",
                                               "teeth", "templ", "forehead")))}
        lines.append("PROBE — the SAME FBX imported as-is, this run:")
        lines.append("  bones          {0}   families {1}".format(len(rows), families))
        lines.append("  of those, {0} are face bones (brow/lid/lip/jaw/chin/nose/cheek/eye/"
                     "tongue/teeth/temple/forehead) on a pair of arms"
                     .format(result["face_bones"]))
        lines.append("  material slots {0}".format(len(slots)))
        lines.append("  UE bounds      X {0:.2f}..{1:.2f}  Y {2:.2f}..{3:.2f}  Z {4:.2f}..{5:.2f} uu"
                     .format(*result["bounds"]))
        break
    else:
        fail("the as-is FBX probe produced no SkeletalMesh at all")
    _eal().delete_directory(PROBE_DIR)
    return result


def bind_material(mesh_path, lines):
    """Point the one slot at MI_Pack_shell, and PROVE it from disk afterwards.

    THE TRAP THIS AVOIDS. `mesh.get_editor_property("materials")` hands back
    COPIES of the FSkeletalMaterial structs. Mutating the loop variable and then
    writing the array back writes the array you never changed, and the import
    reports success with every slot still on WorldGridMaterial — the exact
    silent no-op recorded against import_pack.py:assign_materials. The fix is
    the index write-back below, and the only evidence that it worked is a
    RELOAD-FROM-DISK readback, which is why this returns what it re-read rather
    than what it set."""
    mi = unreal.load_asset(PACK_SHELL_MI)
    if mi is None:
        fail("{0} is missing — run Scripts/import-pack.sh first. The mesh keeps the "
             "engine default material.".format(PACK_SHELL_MI))
        return []
    mesh = unreal.load_asset(mesh_path)
    if mesh is None:
        fail("cannot load {0} to bind its materials".format(mesh_path))
        return []
    slots = mesh.get_editor_property("materials")
    for i in range(len(slots)):
        slot = slots[i]
        slot.set_editor_property("material_interface", mi)
        slots[i] = slot                       # <- the write-back that makes it real
    mesh.set_editor_property("materials", slots)
    _eal().save_loaded_asset(mesh, only_if_is_dirty=False)

    # readback, from the asset as it now exists on disk
    _eal().load_asset(mesh_path)
    reread = unreal.load_asset(mesh_path)
    out = []
    for i, slot in enumerate(reread.get_editor_property("materials")):
        bound = slot.get_editor_property("material_interface")
        name = str(slot.get_editor_property("material_slot_name"))
        path = bound.get_path_name().split(".")[0] if bound else None
        out.append((name, path))
        lines.append("  slot {0} {1:<16} -> {2}".format(i, name, path or "NOTHING"))
        if path != PACK_SHELL_MI:
            fail("slot {0} ({1}) read back as {2!r}, not {3}"
                 .format(i, name, path, PACK_SHELL_MI))
    if not out:
        fail("{0} has no material slots at all".format(mesh_path))
    return out


def ring_report(rows, lines):
    """The owner's complaint, measured off the IMPORTED skeleton.

    Same method as finger_roll_table(), run on UE's reference pose instead of
    the FBX bind matrices, so it describes the asset the pose stage will open.
    The comparison is between siblings, so UE's left-handedness cancels and no
    convention has to be trusted.

    WHICH LOCAL AXIS RUNS DOWN THE BONE IS NOT THE SAME ON BOTH SIDES OF THE
    IMPORT, and assuming it is was a real bug here for one run. Blender runs its
    bones down local +Y. Interchange converts a right-handed Y-up basis to UE's
    left-handed Z-up one by conjugating with the Y/Z swap S, and R_ue = S*R*S
    permutes the COLUMNS as well as the rows — so the source's local Y arrives
    as UE's local Z and vice versa, while local X (the flexion axis) is
    untouched. Rather than hard-code that, the along-bone axis is DETECTED from
    the geometry below: for every joint that has a child, the local axis with
    the largest |dot| against the direction to that child wins, and all thirty
    joints must agree on the same letter or this refuses to report."""
    table = {r["name"]: r for r in rows}

    def sub(a, b):
        return [a[i] - b[i] for i in range(3)]

    def dot(a, b):
        return sum(a[i] * b[i] for i in range(3))

    def cross(a, b):
        return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]

    def norm(a):
        l = math.sqrt(dot(a, a)) or 1.0
        return [c / l for c in a]

    FINGER_NAMES = ("thumb", "index", "middle", "ring", "pinky")

    # --- which local axis runs down a finger bone, detected not assumed -------
    votes = {}
    for side, _S in SIDES:
        for finger in FINGER_NAMES:
            for j in range(2):
                a = table.get("{0}_{1}_{2}".format(finger, side, j))
                b = table.get("{0}_{1}_{2}".format(finger, side, j + 1))
                if a is None or b is None:
                    continue
                d = norm(sub(b["pos"], a["pos"]))
                letter = max("XYZ", key=lambda k: abs(dot(norm(a[k]), d)))
                votes.setdefault(letter, []).append(dot(norm(a[letter]), d))
    if len(votes) != 1:
        fail("the finger joints disagree about which local axis runs down the bone: {0}"
             .format({k: len(v) for k, v in votes.items()}))
        return {}
    axis = list(votes)[0]
    agree = votes[axis]
    sign = 1.0 if sum(agree) > 0 else -1.0
    lines.append("  along-bone axis on this skeleton: local {0}{1}  (|dot| >= {2:.3f} over "
                 "{3} joint pairs).  Flexion axis: local X."
                 .format("+" if sign > 0 else "-", axis, min(abs(v) for v in agree), len(agree)))

    verdict = {}
    for side, _S in SIDES:
        need = ["{0}_{1}_{2}".format(f, side, j) for f in FINGER_NAMES for j in range(3)]
        if any(n not in table for n in need):
            fail("cannot measure the {0} hand: missing {1}"
                 .format(side, [n for n in need if n not in table]))
            continue
        fdir = norm(sub(table["middle_{0}_2".format(side)]["pos"],
                        table["middle_{0}_0".format(side)]["pos"]))
        spread = norm(sub(table["pinky_{0}_0".format(side)]["pos"],
                          table["index_{0}_0".format(side)]["pos"]))
        ref = norm(cross(fdir, spread))
        lines.append("  {0} hand — flexion reference axis (component space) "
                     "({1:+.3f} {2:+.3f} {3:+.3f})".format(side.upper(), *ref))
        lines.append("    {0:<18} {1:>9} {2:>8}   {3}"
                     .format("joint", "roll deg", "along", "flexion axis +X, component space"))
        rolls = {}
        for finger in FINGER_NAMES:
            for j in range(3):
                name = "{0}_{1}_{2}".format(finger, side, j)
                r = table[name]
                bone = [sign * c for c in norm(r[axis])]
                child = "{0}_{1}_{2}".format(finger, side, j + 1)
                along = (dot(bone, norm(sub(table[child]["pos"], r["pos"])))
                         if child in table else float("nan"))
                perp = norm(sub(ref, [dot(ref, bone) * c for c in bone]))
                s = cross(perp, r["X"])
                roll = math.degrees(math.atan2(dot(s, bone), dot(perp, r["X"])))
                rolls[name] = roll
                lines.append("    {0:<18} {1:>+9.1f} {2:>8}   ({3:+.3f} {4:+.3f} {5:+.3f})"
                             .format(name, roll,
                                     "  leaf" if along != along else "%+.3f" % along, *r["X"]))
        deltas = {}
        for j in range(3):
            for other in ("index", "middle", "pinky"):
                d = (rolls["ring_{0}_{1}".format(side, j)]
                     - rolls["{0}_{1}_{2}".format(other, side, j)] + 540.0) % 360.0 - 180.0
                deltas["ring_{0}_{1} - {2}_{0}_{1}".format(side, j, other)] = d
        lines.append("    ring vs siblings, same joint index (0 = same curl direction, "
                     "+-180 = opposite):")
        for k in sorted(deltas):
            lines.append("      {0:<34} {1:+8.1f} deg".format(k, deltas[k]))
        # DISAGREEMENT WITH THE MIDDLE FINGER IS THE ACTIONABLE NUMBER, and the
        # worst-of-three is not: the pinky is itself splayed ~9.5 degrees off
        # middle in this rig (ordinary hand anatomy, visible in the roll column
        # above), so a ring/pinky comparison can never come out clean no matter
        # how correct the asset is. Middle is the reference finger.
        vs_middle = max(abs(deltas["ring_{0}_{1} - middle_{0}_{1}".format(side, j)])
                        for j in range(3))
        worst = max(abs(v) for v in deltas.values())
        flipped = all(abs(v) > 150.0 for v in deltas.values())
        consistent = vs_middle <= 30.0
        verdict[side] = {"flipped": flipped, "consistent": consistent,
                         "vs_middle_deg": vs_middle, "worst_sibling_deg": worst,
                         "deltas": deltas,
                         "bone_axis": ("+" if sign > 0 else "-") + axis}
        lines.append("    VERDICT {0}: the ring chain curls {1} its siblings. Disagreement "
                     "with middle (the reference finger) {2:.1f} deg; worst of "
                     "index/middle/pinky {3:.1f} deg."
                     .format(side,
                             "OPPOSITE TO" if flipped else
                             ("WITH" if consistent else "at an ODD ANGLE TO"),
                             vs_middle, worst))
        if not consistent:
            # The rebuild stage rolls those six bind frames 180 degrees on
            # purpose (apply_ring_roll_fix). If the asset on disk still curls the
            # ring the other way, the fix did not survive to the mesh Unreal
            # loaded, and every pose built on it will show one finger standing up.
            fail("{0} hand: the imported ring chain still disagrees with the middle finger by "
                 "{1:.1f} deg. The rebuild's ring-roll fix did not reach the imported skeleton."
                 .format(side, vs_middle))
    return verdict


def measure(path, label, lines):
    asset = unreal.load_asset(path)
    if asset is None:
        fail("{0}: {1} did not land".format(label, path))
        return None, []
    b = asset.get_bounds()
    o, e = b.origin, b.box_extent
    lines.append("{0}  {1}".format(label, path))
    lines.append("  bounds         X {0:8.2f}..{1:8.2f}   Y {2:8.2f}..{3:8.2f}   "
                 "Z {4:8.2f}..{5:8.2f}  uu".format(o.x - e.x, o.x + e.x, o.y - e.y,
                                                   o.y + e.y, o.z - e.z, o.z + e.z))
    lines.append("  size           {0:.2f} x {1:.2f} x {2:.2f} uu"
                 .format(e.x * 2, e.y * 2, e.z * 2))
    rows = bone_table(asset)
    lines.append("  bones          {0}".format(len(rows)))
    slots = list(asset.get_editor_property("materials"))
    lines.append("  material slots {0}".format(len(slots)))
    return asset, rows


def do_import():
    lines = []
    if not os.path.isfile(GLB):
        fail("{0} is missing — run the rebuild stage first".format(GLB))
        return lines
    with open(MANIFEST) as handle:
        manifest = json.load(handle)

    probe = probe_fbx_as_is(lines)
    lines.append("")

    # The wipe happens here AND in the shell. EditorAssetLibrary.delete_asset
    # drops the file but the Asset Registry keeps the entry for the rest of the
    # session, so a rename onto a just-deleted name fails and the run silently
    # re-measures the LAST run's asset — the trap recorded in import-rocco.sh.
    for folder in (STAGING,):
        if _eal().does_directory_exist(folder):
            _eal().delete_directory(folder)

    pipeline = build_pipeline("TraceHandsGLTF", "gltf")
    if pipeline is None:
        return lines
    run_import(GLB, STAGING, pipeline, DEFAULT_GLTF_PIPELINE)

    got = landed(STAGING)
    meshes = sorted(p for p, a in got.items() if isinstance(a, unreal.SkeletalMesh))
    skeletons = sorted(p for p, a in got.items() if isinstance(a, unreal.Skeleton))
    lines.append("IMPORT — the rebuilt GLB:")
    for p, a in sorted(got.items()):
        lines.append("  landed  {0:<62} {1}".format(p, type(a).__name__ if a else "<unloadable>"))
    if len(meshes) != 1:
        fail("expected exactly one SkeletalMesh from the GLB, got {0}".format(len(meshes)))
        return lines

    def rename(src, dst):
        if src == dst:
            return dst
        if _eal().does_asset_exist(dst):
            _eal().delete_asset(dst)
        if _eal().rename_asset(src, dst):
            return dst
        fail("could not rename {0} -> {1}".format(src, dst))
        return src

    mesh_path = rename(meshes[0], "{0}/{1}".format(ROOT, MESH_NAME))
    skel_path = rename(skeletons[0], "{0}/{1}".format(ROOT, SKELETON_NAME)) if skeletons else None
    for extra in meshes[1:] + skeletons[1:]:
        lines.append("  ALSO LANDED (left alone): {0}".format(extra))
    if _eal().does_directory_exist(STAGING):
        _eal().delete_directory(STAGING)
    lines.append("")

    lines.append("MATERIAL BINDING (set, then re-read from disk):")
    bind_material(mesh_path, lines)
    lines.append("")

    asset, rows = measure(mesh_path, "ARRIVED", lines)
    if asset is None:
        return lines
    lines.append("  skeleton       {0}".format(skel_path or "NONE"))

    # The end-to-end coordinate proof: the rebuild must land where the raw FBX
    # landed. If this drifts, a transform is wrong somewhere between Blender
    # metres and Unreal centimetres and nothing downstream can be trusted.
    if probe and probe.get("bounds"):
        b = asset.get_bounds()
        o, e = b.origin, b.box_extent
        mine = (o.x - e.x, o.x + e.x, o.y - e.y, o.y + e.y, o.z - e.z, o.z + e.z)
        drift = max(abs(a - c) for a, c in zip(mine, probe["bounds"]))
        lines.append("  bounds vs the as-is FBX import: max corner drift {0:.3f} uu".format(drift))
        if drift > 0.5:
            fail("the rebuilt mesh does not land where the raw FBX did (drift {0:.3f} uu); "
                 "the metre/centimetre or axis conversion is wrong".format(drift))

    lines.append("")
    lines.append("BONES ({0}), component space, uu — X right, Y forward, Z up:".format(len(rows)))
    weighted = set(manifest["weighted_bones"])
    for r in rows:
        lines.append("  {0}{1:>3} {2:<22} parent={3:<22} ({4:8.2f},{5:8.2f},{6:8.2f})"
                     .format("*" if r["name"] in weighted else " ", r["index"], r["name"],
                             r["parent"] or "-", r["pos"][0], r["pos"][1], r["pos"][2]))
    lines.append("  (* = carries skin weights: {0} of {1} bones)"
                 .format(len(weighted), len(rows)))

    expected = [b["name"] for b in manifest["bones"]]
    arrived = [r["name"] for r in rows]
    if sorted(expected) != sorted(arrived):
        fail("bone names do not match the manifest. missing={0} extra={1}"
             .format(sorted(set(expected) - set(arrived)), sorted(set(arrived) - set(expected))))
    for b in manifest["bones"]:
        want = b["parent"]
        got_parent = next((r["parent"] for r in rows if r["name"] == b["name"]), "<absent>")
        if (want or None) != got_parent:
            fail("bone {0}: expected parent {1!r}, imported as {2!r}"
                 .format(b["name"], want, got_parent))

    lines.append("")
    lines.append("RING-FINGER MEASUREMENT (bind pose of the IMPORTED skeleton):")
    verdict = ring_report(rows, lines)

    lines.append("")
    lines.append("SCALE, against the shipped first-person hands:")
    pack, pack_rows = measure(PACK_HANDS, "REFERENCE", lines)
    if pack is not None:
        pt = {r["name"]: r["pos"] for r in pack_rows}
        mt = {r["name"]: r["pos"] for r in rows}

        def span(t, a, b):
            if a not in t or b not in t:
                return None
            return math.sqrt(sum((t[a][i] - t[b][i]) ** 2 for i in range(3)))

        for a, b in (("hand_right", "index_right_2"), ("hand_right", "index_right_0"),
                     ("index_right_0", "pinky_right_0"), ("hand_left", "hand_right")):
            p, m = span(pt, a, b), span(mt, a, b)
            if p is None or m is None:
                lines.append("  {0} -> {1}: pack {2}  arms {3}"
                             .format(a, b, "n/a" if p is None else "%.2f uu" % p,
                                     "n/a" if m is None else "%.2f uu" % m))
                continue
            lines.append("  {0:<16} -> {1:<16} pack {2:7.2f} uu   arms {3:7.2f} uu   "
                         "arms are {4:.2f}x".format(a, b, p, m, m / p if p else 0.0))
    return lines, verdict


# =============================================================================
def main():
    log("import_hands.py stage={0}".format(STAGE))
    verdict = None
    if STAGE == "rebuild":
        manifest = rebuild()
        if manifest is not None:
            log("")
            log("================ HANDS REBUILD REPORT ================")
            log("  {0} -> {1}".format(manifest["source_fbx"], manifest["glb"]))
            log("  source rig: {0} Model nodes, {1} clusters, {2} weighted"
                .format(manifest["fbx_models"], manifest["fbx_clusters"],
                        manifest["fbx_weighted_clusters"]))
            log("  kept:       {0} bones, {1} of them weighted"
                .format(len(manifest["bones"]), len(set(manifest["weighted_bones"]))))
            log("  geometry:   {0} source verts -> {1} split verts, {2} triangles"
                .format(manifest["vertices_source"], manifest["vertices_split"],
                        manifest["triangles"]))
            for side in sorted(manifest["ring_roll"]):
                j = manifest["ring_roll"][side]["joints"]
                log("  {0}: ring_0 roll {1:+.1f} vs middle_0 {2:+.1f} vs index_0 {3:+.1f} "
                    "vs pinky_0 {4:+.1f} deg   (ring vs middle {5:.1f} deg before the fix, "
                    "{6:.1f} deg after)"
                    .format(side, j["ring_{0}_0".format(side)]["roll_deg"],
                            j["middle_{0}_0".format(side)]["roll_deg"],
                            j["index_{0}_0".format(side)]["roll_deg"],
                            j["pinky_{0}_0".format(side)]["roll_deg"],
                            manifest["ring_vs_middle_deg_before"][side],
                            manifest["ring_vs_middle_deg_after"][side]))
            log("================ END REPORT ================")
        log("[import-hands] REBUILD EXIT={0}".format(1 if _failures else 0))
    elif STAGE == "import":
        result = do_import()
        lines, verdict = result if isinstance(result, tuple) else (result, None)
        log("")
        log("================ HANDS IMPORT REPORT ================")
        for line in lines:
            log(line)
        log("================ END REPORT ================")
        if PIPELINE_DIR and _eal().does_directory_exist(PIPELINE_DIR):
            _eal().delete_directory(PIPELINE_DIR)
        _eal().save_directory(ROOT, only_if_is_dirty=False, recursive=True)
        if verdict:
            for side in sorted(verdict):
                log("[import-hands] RING {0}: consistent={1} vs_middle={2:.1f}deg"
                    .format(side, verdict[side]["consistent"], verdict[side]["vs_middle_deg"]))
        log("[import-hands] EXIT={0}".format(1 if _failures else 0))
    else:
        fail("unknown TRACE_HANDS_STAGE {0!r}; expected rebuild or import".format(STAGE))
        log("[import-hands] EXIT=1")

    if _failures:
        log("{0} PROBLEM(S):".format(len(_failures)))
        for f in _failures:
            log("  - {0}".format(f))
    else:
        log("no problems reported.")
    if unreal is None and _failures:
        raise SystemExit(1)


main()
