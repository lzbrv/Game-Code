# =============================================================================
# Trace - generate-input-assets.py
#
# Writes the Enhanced Input CONTENT out of C++ and into real .uassets at
# /Game/Trace/Input:
#
#     IA_Move IA_Look IA_Jump IA_Crouch IA_Fire IA_Pass IA_Dash IA_Parry
#     IA_Scoreboard IA_EquipKnife IA_EquipGun IA_EquipSmg IA_Ability
#     IA_AbilitySecondary IA_Reload IA_PullCore IA_Melee
#                                                      (17 UInputAction assets)
#     IMC_Trace                                        (1 UInputMappingContext)
#
# -----------------------------------------------------------------------------
# WHY THIS EXISTS  (spec v17 section 6)
# -----------------------------------------------------------------------------
# ATracePlayerController::BuildInputData builds every UInputAction and the one
# UInputMappingContext with NewObject at runtime. That works, and it is still
# there as the live fallback, but it means nobody can open an input action,
# nobody can see the mapping table without reading C++, and nothing outside the
# controller can reference an action.
#
# THIS IS A REORGANISATION, NOT A REDESIGN. The values written here are read out
# of the SAME table the C++ path uses, so a fresh install plays identically
# whether the assets are present or not. If you change a default key in
# Source/Trace/Settings/TraceUserSettings.cpp, re-run this script; if you forget,
# `Trace.Input.VerifyAssets` says so out loud (and the game still plays, because
# the runtime binds come from UTraceUserSettings either way - see below).
#
# -----------------------------------------------------------------------------
# WHAT IS AND IS NOT AUTHORITATIVE  (read this before editing an asset by hand)
# -----------------------------------------------------------------------------
# AUTHORITATIVE, and genuinely used by the running game:
#   * the 15 IA_* assets themselves - the controller binds its handlers to THESE
#     objects when they exist, and takes ValueType and AccumulationBehavior from
#     them. Get one of those wrong and input silently reads as zeroes, which is
#     why the controller validates them and falls back to C++ if they disagree.
#   * IMC_Trace's identity and its asset-level settings (description, tracking
#     mode). The controller duplicates the loaded asset per player, so nothing
#     the game does at runtime can dirty or mutate the asset on disk.
#
# NOT AUTHORITATIVE:
#   * IMC_Trace's key mapping list. The player's own binds live in
#     Saved/Config/<Platform>/TraceUserSettings.ini and the SHIPPED DEFAULTS live
#     in the action table in Source/Trace/Settings/TraceUserSettings.cpp. The
#     controller rebuilds the mapping list from those on every settings change,
#     so re-keying IMC_Trace in the editor changes what you SEE, not what you
#     PLAY. The mappings are written here so the asset is an honest picture of
#     the defaults - and `Trace.Input.VerifyAssets` fails loudly the moment the
#     picture and the table disagree.
#
# -----------------------------------------------------------------------------
# HOW TO RUN IT
# -----------------------------------------------------------------------------
#     UE="/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor"
#     "$UE" "$PWD/Trace.uproject" \
#         -run=pythonscript -script="$PWD/Scripts/generate-input-assets.py" \
#         -unattended -nosplash -NullRHI -nosound \
#         -log -abslog="$PWD/Saved/Logs/trace-generate-input-assets.log"
#
# -NullRHI is safe here (unlike the first arena bake): input assets compile no
# shaders. Or paste it into the editor's Output Log Python prompt:
#
#     exec(open("/path/to/Scripts/generate-input-assets.py").read())
#
# OPTIONS (environment variables - the pythonscript commandlet has no clean way
# to pass argv through -script=)
#   TRACE_INPUT_DIR    package dir. Default /Game/Trace/Input
#   TRACE_INPUT_KEEP   "1" to leave existing assets alone instead of rewriting
#                      them. Default off: a regenerate REWRITES every field of
#                      every asset in place. Re-running is safe and idempotent.
# =============================================================================

import os
import sys

try:
    import unreal
except ImportError:  # pragma: no cover - only possible outside the editor
    sys.stderr.write(
        "generate-input-assets.py must run inside Unreal Editor's Python\n"
        "environment. Use the -run=pythonscript command line in this file's\n"
        "header.\n"
    )
    raise SystemExit(2)


INPUT_DIR = os.environ.get("TRACE_INPUT_DIR", "/Game/Trace/Input").rstrip("/")
KEEP = os.environ.get("TRACE_INPUT_KEEP", "0") == "1"

Failures = []


def log(message):
    unreal.log("[InputAssets] {0}".format(message))


def fail(message):
    Failures.append(message)
    unreal.log_error("[InputAssets] {0}".format(message))


# -----------------------------------------------------------------------------
# 1. The action table.
#
# MIRRORS ATracePlayerController::BuildInputData EXACTLY, and the ValueType
# column is the load-bearing one: an Axis2D action read as a Boolean does not
# error, it silently delivers zeroes. IA_Move's Cumulative accumulation is the
# other one - the default (TakeHighestAbsoluteValue) makes W+S walk backwards
# and counter-strafing impossible, which is a bug this project has already had.
# -----------------------------------------------------------------------------
BOOL = unreal.InputActionValueType.BOOLEAN
AXIS2D = unreal.InputActionValueType.AXIS2D

CUMULATIVE = unreal.InputActionAccumulationBehavior.CUMULATIVE
HIGHEST = unreal.InputActionAccumulationBehavior.TAKE_HIGHEST_ABSOLUTE_VALUE

ACTIONS = [
    # (asset name, value type, accumulation, editor description)
    ("IA_Move", AXIS2D, CUMULATIVE,
     "Movement. X = strafe (+right), Y = forward (+forward). Four 1D keys are "
     "swizzled and negated into one Axis2D; Cumulative accumulation is what lets "
     "opposing keys cancel."),
    ("IA_Look", AXIS2D, HIGHEST,
     "Mouse look. X = yaw delta, Y = pitch delta. The per-axis Scalar modifiers "
     "carry the player's sensitivity, and the SIGN of the Y scalar is the "
     "invert-Y setting."),
    ("IA_Jump", BOOL, HIGHEST, "Jump. Held: the release edge stops the jump."),
    ("IA_Crouch", BOOL, HIGHEST, "Crouch. Held: slide on the ground, fast-fall in the air."),
    ("IA_Fire", BOOL, HIGHEST,
     "Fire. Also swings the knife (the weapon component branches on which weapon "
     "is out) and doubles as 'put me back in' while dead. While CARRYING the Core "
     "this is the throw (goals) or the hover-pass (endzones) - spec v25 s7 leaves "
     "that untouched, which is why the throw needs no default key of its own."),
    ("IA_Pass", BOOL, HIGHEST,
     "Throw / pass the Core. SPEC v25 s7: NO DEFAULT KEY - it used to be the right "
     "mouse button and that bind is removed. It has no row in MAPPINGS below for "
     "exactly that reason. Mouse 1 throws while carrying; this action is the "
     "optional second route and stays on the rebind list so a player can bind one."),
    ("IA_Dash", BOOL, HIGHEST, "Dash."),
    ("IA_Parry", BOOL, HIGHEST,
     "Parry. Carrier only; a 0.175 s window of trace invulnerability. SPEC v28 s3d "
     "gives it TWO default keys - Q and the thumb mouse button - and takes it OFF "
     "right click, which spec v28 s10 needs for the melee. Two mappings on one "
     "Boolean action is a supported shape: Enhanced Input merges them by highest "
     "absolute value, so the action is down while EITHER key is."),
    ("IA_Scoreboard", BOOL, HIGHEST, "Scoreboard. Shown while held."),
    # SPEC v29 s5 - THREE weapon states and the knife is in all of them:
    #   1 stows the GUNS (knife only, and the only state that pays the v12 s3
    #     speed boost), 2 is the pistol, 3 is the SMG.
    # The two older ASSET NAMES keep their v13 spellings because
    # ETraceInputAction does - renaming the enumerator would break another
    # slice's file. The meaning moved; the spelling did not. The DisplayName
    # and the ConfigId in TraceInputActions::All() are the strings that reach a
    # player, and both of those DID move ("StowGuns", "EquipPistol").
    ("IA_EquipKnife", BOOL, HIGHEST,
     "SPEC v29 s5: STOW THE GUNS - knife only, and the only state that pays the "
     "v12 s3 movement boost. DIRECT SELECT and idempotent - not a toggle."),
    ("IA_EquipGun", BOOL, HIGHEST,
     "SPEC v29 s5: pull out the PISTOL. The knife stays in the off hand. DIRECT "
     "SELECT and idempotent - not a toggle."),
    ("IA_EquipSmg", BOOL, HIGHEST,
     "SPEC v29 s5: pull out the SMG. The knife stays in the off hand. DIRECT "
     "SELECT and idempotent - not a toggle."),
    ("IA_Ability", BOOL, HIGHEST, "Activated ability. A press."),
    ("IA_AbilitySecondary", BOOL, HIGHEST, "Secondary ability. A HOLD - Mace suspends only while it is down."),
    ("IA_Reload", BOOL, HIGHEST, "Reload. The clip also reloads itself when it empties."),
    # SPEC v26 s1 - "Make parry and pull core two separate binds in the settings
    # menu." Its OWN action, not a second key on IA_Parry: the keybind page lists
    # ACTIONS, so a second key on the parry would be a bind nobody could see or
    # change on its own. A HOLD - the controller binds Started, Completed AND
    # Canceled, because spec v25 s2's rule is that releasing cancels the pull.
    ("IA_PullCore", BOOL, HIGHEST,
     "Pull the Core during a turnover (spec v25 s2). HELD - releasing cancels. "
     "Spec v26 s1 split this off the parry's button into a bind of its own."),
    # SPEC v28 s10 - "Melee should be bound to right click by default." The whole
    # verb is TraceMelee::HandleMeleeInput; this action only delivers edges to it.
    # Bound Started AND Completed AND Canceled, because the press may go to the
    # Core PULL, which is a HOLD - a dropped release leaves a ring filling on the
    # server. The precedence between the swing and the pull is a state test inside
    # the verb (ATraceCore::CanPullNow, the same predicate the HUD asks to decide
    # whether to draw the circle), never a second mapping here.
    ("IA_Melee", BOOL, HIGHEST,
     "Melee swing (spec v28 s10). Right mouse by default. If and only if the pull "
     "circle is on screen this press pulls the Core instead - that precedence "
     "lives in TraceMelee::HandleMeleeInput, not in the mapping."),
]

# -----------------------------------------------------------------------------
# 2. The default mapping table.
#
# THE ORDER IS THE ORDER ATracePlayerController::ApplyControlSettings LAYS THEM
# DOWN, and it is copied rather than sorted for a reason: Enhanced Input merges
# two mappings of the same action with `>=`, so on a tie the LAST mapping added
# wins. IA_Move is Cumulative and does not care, but the next person to add a
# second key to a Boolean action would.
#
# The keys are the shipped defaults from TraceInputActions::All(). A player who
# has rebound something gets THEIR key at runtime; this table is what a fresh
# install starts from, which is exactly what "generated from the current action
# table so the defaults are unchanged" means.
#
# AN ACTION WITH NO DEFAULT KEY HAS NO ROW HERE, and since spec v25 s7 there is
# one: IA_Pass. An action with TWO default keys has TWO rows, and since spec v28
# s3d there is one of those too: IA_Parry. So the mapping count is neither the
# action count nor anything derivable from it - it is asserted against
# ATracePlayerController's own ExpectedMappings table by Trace.Input.VerifyAssets.
# -----------------------------------------------------------------------------
SWIZZLE_XY = "swizzle"   # 1D X -> Y, so a single key becomes the forward axis
NEGATE = "negate"
SCALAR_LOOK_X = "scalar_x"
SCALAR_LOOK_Y = "scalar_y"

# Shipped mouse defaults, from UTraceUserSettings: MouseSensitivity 1.5,
# MouseSensitivityYScale 1.0, bInvertMouseY false -> both scalars are 1.5.
DEFAULT_LOOK_SCALE_X = 1.50
DEFAULT_LOOK_SCALE_Y = 1.50

MAPPINGS = [
    # (action asset name, key name, [modifiers], comment)
    ("IA_Move", "W", [SWIZZLE_XY], "forward"),
    ("IA_Move", "S", [SWIZZLE_XY, NEGATE], "back"),
    ("IA_Move", "A", [NEGATE], "strafe left"),
    ("IA_Move", "D", [], "strafe right"),
    ("IA_Look", "MouseX", [SCALAR_LOOK_X], "yaw"),
    ("IA_Look", "MouseY", [SWIZZLE_XY, SCALAR_LOOK_Y], "pitch"),
    ("IA_Jump", "SpaceBar", [], ""),
    ("IA_Crouch", "LeftControl", [], ""),
    ("IA_Fire", "LeftMouseButton", [], ""),
    # SPEC v25 s7: IA_Pass HAS NO ROW. It sat here on RightMouseButton until v25
    # removed that bind; the throw now ships UNBOUND (Default_Pass() returns an
    # invalid FKey) and an unbound action gets no mapping anywhere - not in
    # ApplyControlSettings, which skips invalid keys, and not here, because there
    # is no key name to write. ATracePlayerController's ExpectedMappings table
    # has had the matching row removed, so Trace.Input.VerifyAssets agrees.
    ("IA_Dash", "LeftShift", [], ""),
    # SPEC v25 s7 moved parry from Q to the right mouse button; SPEC v26 s1 split the
    # pull off it; SPEC v28 s3d moves it BACK to Q and gives it a SECOND key on the
    # thumb mouse button - "Parry defaults to BOTH Q and the thumb mouse button".
    #
    # TWO ROWS FOR ONE ACTION, and this is the first time this table has had that.
    # It is the picture of an action with two slots (UTraceUserSettings holds up to
    # MaxKeysPerAction=2 per action since v28 s3c), and ApplyControlSettings lays
    # them down in the same order from the player's own binds.
    #
    # THE RIGHT MOUSE BUTTON BELONGS TO THE MELEE, and to exactly one row. Spec v28
    # s3d took it off the parry (see the two IA_Parry rows below) so that spec v28
    # s10 could have it. Melee IS a rebindable action - ETraceInputAction::Melee,
    # added when the two slices were integrated - so its row is at the bottom of
    # this table. Nothing else here may sit on the button, or one press would do
    # two things.
    ("IA_Parry", "Q", [], "parry, slot 1 (spec v28 s3d)"),
    ("IA_Parry", "ThumbMouseButton", [], "parry, slot 2 - mouse 4 (spec v28 s3d)"),
    ("IA_Scoreboard", "Tab", [], ""),
    ("IA_EquipKnife", "One", [], "spec v29 s5 - STOW GUNS (knife only, +22% speed)"),
    ("IA_EquipGun", "Two", [], "spec v29 s5 - the pistol"),
    ("IA_Ability", "E", [], ""),
    ("IA_AbilitySecondary", "V", [], ""),
    ("IA_Reload", "R", [], ""),
    # SPEC v26 s1. F, and the collision check is the only decision there was:
    # WASD, Space, LeftControl, LeftShift, both mouse buttons, Tab, 1, 2, E, V and
    # R are claimed; F was vacated by spec v15 s5 when the SwapWeapon toggle was
    # deleted, and "F to grab the thing you are looking at" is this genre's
    # strongest convention. Q (freed when parry moved to the mouse) was the other
    # candidate - passed over because the pull is a HOLD taken while still
    # steering, and F is under an index finger already resting on D.
    ("IA_PullCore", "F", [], "spec v26 s1 - the turnover core-pull, its own bind"),
    # SPEC v28 s10 - the melee, on the button spec v28 s3d vacated for it. LAST in
    # this table because ATracePlayerController::ApplyControlSettings lays it down
    # last, and this table is a copy of that order rather than a sort - see the
    # note at the top of the section.
    ("IA_Melee", "RightMouseButton", [], "spec v28 s10 - melee, on the button s3d vacated"),
    # SPEC v29 s5 - "3 pulls out smg". LAST in this table because
    # ATracePlayerController::ApplyControlSettings lays it down last, and this
    # table is a copy of that order rather than a sort - see the note at the top
    # of the section. Nothing else in the action table claims a digit, so this
    # default steals no key on a first run.
    ("IA_EquipSmg", "Three", [], "spec v29 s5 - the SMG"),
]

CONTEXT_NAME = "IMC_Trace"
CONTEXT_DESCRIPTION = (
    "Trace's one and only mapping context. GENERATED by "
    "Scripts/generate-input-assets.py - hand edits are lost on the next run. The "
    "key list below is the SHIPPED DEFAULT and is a picture, not a control: "
    "ATracePlayerController duplicates this asset per player and rebuilds the "
    "mapping list from UTraceUserSettings, so the player's own binds always win. "
    "To change a default, change the action table in "
    "Source/Trace/Settings/TraceUserSettings.cpp and re-run the script."
)


# -----------------------------------------------------------------------------
# 3. Helpers
# -----------------------------------------------------------------------------

def package_path(name):
    return "{0}/{1}".format(INPUT_DIR, name)


def set_optional(obj, prop, value):
    """set_editor_property that survives a property this engine build does not have."""
    try:
        obj.set_editor_property(prop, value)
        return True
    except Exception as error:  # noqa: BLE001 - the point is to not abort the run
        log("  note: could not set '{0}' on {1}: {2}".format(prop, obj.get_name(), error))
        return False


def make_asset(name, asset_class):
    """
    Return the asset at INPUT_DIR/name, ready to have every field rewritten.

    REWRITTEN IN PLACE WHEN IT ALREADY EXISTS, and that is not a shortcut - it is
    the only correct behaviour, for two reasons this script learned the hard way:

      1. delete_asset() + create_asset() IN THE SAME SESSION DOES NOT WORK. The
         deleted package is still resident, so the second create_asset returns
         None for every asset and the run aborts having written nothing. That is
         exactly what a re-run does, so the delete-then-create version was broken
         for the ONLY case a person ever hits.
      2. IMC_Trace holds hard references to the fourteen IA_ assets. Deleting and
         recreating an action gives it a new object, which would leave the mapping
         context pointing at nothing. Rewriting keeps the identity and therefore
         keeps the references.

    Every field this script cares about is assigned unconditionally by the caller,
    so a rewrite is a full regeneration and not a merge.
    """
    path = package_path(name)
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        existing = unreal.EditorAssetLibrary.load_asset(path)
        if KEEP:
            log("keeping existing {0}".format(path))
            return existing

        if existing is not None and existing.get_class().get_name() == asset_class.static_class().get_name():
            log("rewriting existing {0}".format(path))
            return existing

        # Wrong class under the right name - the only case that genuinely needs a
        # delete, and it cannot be rewritten into the right thing.
        fail("{0} exists but is a {1}, not a {2}. Delete it by hand and re-run.".format(
            path,
            existing.get_class().get_name() if existing is not None else "<unloadable>",
            asset_class.static_class().get_name()))
        return None

    tools = unreal.AssetToolsHelpers.get_asset_tools()
    # No factory: UInputAction and UInputMappingContext have no Python-exposed
    # factory in 5.8 (checked - unreal.InputActionFactory does not exist), and
    # create_asset(..., None) makes a plain default-constructed object of the
    # class, which is exactly what the editor's own factories do for these two.
    asset = tools.create_asset(name, INPUT_DIR, asset_class, None)
    if asset is None:
        fail("create_asset returned None for {0}".format(path))
    return asset


def make_modifier(outer, kind):
    if kind == SWIZZLE_XY:
        modifier = unreal.new_object(unreal.InputModifierSwizzleAxis, outer=outer)
        set_optional(modifier, "order", unreal.InputAxisSwizzle.YXZ)
        return modifier
    if kind == NEGATE:
        # bX/bY/bZ all default to true. Every mapping this is attached to carries
        # a single non-zero component (each key is 1D and any swizzle has already
        # run), so a blanket negate is the whole story.
        return unreal.new_object(unreal.InputModifierNegate, outer=outer)
    if kind in (SCALAR_LOOK_X, SCALAR_LOOK_Y):
        scale = DEFAULT_LOOK_SCALE_X if kind == SCALAR_LOOK_X else DEFAULT_LOOK_SCALE_Y
        modifier = unreal.new_object(unreal.InputModifierScalar, outer=outer)
        set_optional(modifier, "scalar", unreal.Vector(scale, scale, 1.0))
        return modifier
    fail("unknown modifier kind '{0}'".format(kind))
    return None


# -----------------------------------------------------------------------------
# 4. Generate
# -----------------------------------------------------------------------------

def main():
    log("=" * 70)
    log("spec v17 s6 - Enhanced Input becomes real assets")
    log("target directory: {0}".format(INPUT_DIR))
    log("=" * 70)

    if not unreal.EditorAssetLibrary.does_directory_exist(INPUT_DIR):
        unreal.EditorAssetLibrary.make_directory(INPUT_DIR)
        log("created {0}".format(INPUT_DIR))

    # --- the actions ---------------------------------------------------------
    actions = {}
    for name, value_type, accumulation, description in ACTIONS:
        asset = make_asset(name, unreal.InputAction)
        if asset is None:
            continue
        asset.set_editor_property("value_type", value_type)
        asset.set_editor_property("accumulation_behavior", accumulation)
        set_optional(asset, "action_description", description)
        # No triggers: an action with no explicit trigger uses the implicit
        # "down" trigger, which is what gives Started on press, Triggered while
        # held and Completed on release. That is the shape every handler in
        # ATracePlayerController expects, and adding a trigger here would change
        # behaviour, which this pass is not allowed to do.
        asset.set_editor_property("triggers", [])
        asset.set_editor_property("modifiers", [])
        actions[name] = asset
        log("  {0:<22} value_type={1} accumulation={2}".format(
            name, value_type.name, accumulation.name))

    # --- the mapping context -------------------------------------------------
    context = make_asset(CONTEXT_NAME, unreal.InputMappingContext)
    if context is None:
        fail("no mapping context; aborting")
        return 1

    set_optional(context, "context_description", CONTEXT_DESCRIPTION)

    mappings = []
    for action_name, key_name, modifier_kinds, comment in MAPPINGS:
        action = actions.get(action_name)
        if action is None:
            fail("mapping references unknown action '{0}'".format(action_name))
            continue

        # unreal.Key takes NO constructor arguments in 5.8 (checked: it raises
        # "call() takes at most 0 arguments"), and its KeyName is reachable only
        # through the editor-property channel because FKey has a custom
        # ExportTextItem. Default-construct, then set.
        key = unreal.Key()
        key.set_editor_property("key_name", key_name)

        mapping = unreal.EnhancedActionKeyMapping()
        mapping.set_editor_property("action", action)
        mapping.set_editor_property("key", key)
        modifiers = [make_modifier(context, kind) for kind in modifier_kinds]
        mapping.set_editor_property("modifiers", [m for m in modifiers if m is not None])
        mapping.set_editor_property("triggers", [])
        mappings.append(mapping)

        log("  {0:<22} <- {1:<18} {2}{3}".format(
            action_name, key_name,
            "+".join(modifier_kinds) if modifier_kinds else "-",
            "  ({0})".format(comment) if comment else ""))

    # 5.8 moved the live list from the deprecated `Mappings` array into
    # DefaultKeyMappings.Mappings (UInputMappingContext::PostLoad migrates old
    # assets). Writing the new one directly means the asset needs no migration.
    data = unreal.InputMappingContextMappingData()
    data.set_editor_property("mappings", mappings)
    context.set_editor_property("default_key_mappings", data)

    # Cleared explicitly, because a rewrite must not inherit state from the
    # previous asset: a per-profile override left over from a hand edit would sit
    # there invisibly and take precedence over the list written above.
    set_optional(context, "mapping_profile_overrides", {})

    log("{0}: {1} mapping(s)".format(CONTEXT_NAME, len(mappings)))

    # --- save ----------------------------------------------------------------
    for asset in list(actions.values()) + [context]:
        if not unreal.EditorAssetLibrary.save_loaded_asset(asset, only_if_is_dirty=False):
            fail("failed to save {0}".format(asset.get_path_name()))

    # --- verify what actually reached disk -----------------------------------
    #
    # Re-load every asset by PATH rather than trusting the objects above: the
    # thing the game does at runtime is LoadObject on a path, and an asset that
    # exists in memory but did not serialise is exactly the failure this pass
    # must not ship silently.
    log("-" * 70)
    log("verifying from disk")
    for name, value_type, accumulation, _description in ACTIONS:
        path = package_path(name)
        loaded = unreal.EditorAssetLibrary.load_asset(path)
        if loaded is None:
            fail("{0} did not reload".format(path))
            continue
        got_type = loaded.get_editor_property("value_type")
        got_accum = loaded.get_editor_property("accumulation_behavior")
        if got_type != value_type:
            fail("{0} value_type is {1}, expected {2}".format(name, got_type, value_type))
        if got_accum != accumulation:
            fail("{0} accumulation is {1}, expected {2}".format(name, got_accum, accumulation))

    loaded_context = unreal.EditorAssetLibrary.load_asset(package_path(CONTEXT_NAME))
    if loaded_context is None:
        fail("{0} did not reload".format(package_path(CONTEXT_NAME)))
    else:
        reloaded = loaded_context.get_editor_property("default_key_mappings")
        reloaded_mappings = reloaded.get_editor_property("mappings")
        if len(reloaded_mappings) != len(MAPPINGS):
            fail("{0} reloaded with {1} mapping(s), expected {2}".format(
                CONTEXT_NAME, len(reloaded_mappings), len(MAPPINGS)))
        else:
            for index, mapping in enumerate(reloaded_mappings):
                want_action, want_key, want_modifiers, _c = MAPPINGS[index]
                got_action = mapping.get_editor_property("action")
                got_key = mapping.get_editor_property("key")
                got_modifiers = mapping.get_editor_property("modifiers")
                if got_action is None or got_action.get_name() != want_action:
                    fail("mapping {0}: action is {1}, expected {2}".format(
                        index, got_action, want_action))
                if got_key.get_editor_property("key_name") != want_key:
                    fail("mapping {0}: key is {1}, expected {2}".format(
                        index, got_key, want_key))
                if len(got_modifiers) != len(want_modifiers):
                    fail("mapping {0} ({1} <- {2}): {3} modifier(s) survived, expected {4}".format(
                        index, want_action, want_key, len(got_modifiers), len(want_modifiers)))
            log("  {0}: {1} mapping(s) round-tripped".format(
                CONTEXT_NAME, len(reloaded_mappings)))

    log("=" * 70)
    if Failures:
        for message in Failures:
            unreal.log_error("[InputAssets] FAILED: {0}".format(message))
        unreal.log_error("[InputAssets] VERDICT: {0} failure(s). The assets are NOT trustworthy; "
                         "the game will fall back to the C++ input path.".format(len(Failures)))
        return 1

    log("VERDICT: {0} action asset(s) + {1} written and verified under {2}.".format(
        len(ACTIONS), CONTEXT_NAME, INPUT_DIR))
    log("Next: launch the game and run `Trace.Input.VerifyAssets` in the console.")
    log("=" * 70)
    return 0


EXIT_CODE = main()
if EXIT_CODE != 0:
    raise SystemExit(EXIT_CODE)
