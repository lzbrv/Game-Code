#!/usr/bin/env python3
# =============================================================================
# Trace — check-engine-member-shadowing.py
#
# Fails when a local variable or parameter is named after a well-known engine
# base-class member, e.g.
#
#     const AController* Owner = Cast<AController>(GetOwner());
#
# inside an AActor subclass, where AActor::Owner already exists.
#
# WHY THIS EXISTS — AND WHY WE CANNOT JUST TURN THE COMPILER WARNING ON
#   MSVC treats this as an ERROR in this project's warning configuration:
#
#     error C4458: declaration of 'Owner' hides class member
#     note: see declaration of 'AActor::Owner'
#
#   macOS CANNOT catch it, structurally. UnrealBuildTool hard-disables shadow
#   warnings across the clang 17-18.1.3 range regardless of the project's
#   ShadowVariableWarningLevel — see ApplyWarningsAttribute.cs, "No matter what
#   our ShadowVariableWarningLevel is, in the clang 17-18.1.3 range we always
#   disable" — and Apple clang reports itself as 17.0.0. So the setting in
#   Trace.Build.cs is a documented no-op here, this compiles clean, and every
#   Windows developer is stopped.
#
#   This is the FOURTH Windows-only break in this project, and like the other
#   three it was found by a collaborator rather than by us.
#
# WHAT IT CHECKS
#   A curated list of engine members that are genuinely dangerous to shadow and
#   almost never a reasonable local name. It deliberately does NOT include
#   common words like `Name`, `Location` or `Rotation`: the point is a gate that
#   stays clean, because a check that cries wolf gets skipped and then it is
#   worth nothing.
#
#   *** IT IS SCOPE-AWARE, AND IT HAS TO BE. *** These names are members of
#   AActor, not of UObject or UActorComponent. A `UActorComponent` method with a
#   parameter called `Instigator` shadows NOTHING and MSVC says nothing about it
#   — this project has 22 such lines and every one is fine. Only code inside an
#   AActor-derived class can trigger C4458 on them. The first draft of this
#   script ignored that and reported 23 findings where exactly 1 was real; a
#   gate with a 96% false-positive rate would have been turned off within a day.
#
#   Scope is decided by Unreal's own naming convention: `AFoo::Bar()` in a .cpp,
#   or a `class ... AFoo : public AActor`-style declaration in a header, puts us
#   in actor scope. U-prefixed and F-prefixed types do not.
#
#   This is a text scan, not a C++ parser. It looks for a declaration whose
#   declared identifier is one of the names below, skipping comments, strings
#   and member accesses (`Foo->Owner`, `Foo.Owner`, `AActor::Owner`).
# =============================================================================
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE = os.path.join(ROOT, "Source")

# Members of AActor / UActorComponent / UObject that MSVC will flag. Curated to
# names that are real hazards and rarely legitimate locals.
SHADOWED = {
    "Owner", "Role", "RemoteRole", "Instigator", "RootComponent",
    "InputComponent", "PrimaryActorTick", "PrimaryComponentTick",
    "CustomTimeDilation", "NetDormancy", "NetCullDistanceSquared",
    "ReplicatedMovement", "AttachmentReplication", "Children",
    "ControllerRotation", "SpawnCollisionHandlingMethod",
}

# A declaration: optional const/static/etc, a type expression, then the name,
# then '=' or ';' or ')' or ','. Requires a type before the name, so `Owner = X;`
# (an assignment to the real member) is not flagged.
DECL_RE = re.compile(
    r"(?:^|[;{}(,]|\bconst\b|\bstatic\b)\s*"
    r"(?:const\s+|volatile\s+|struct\s+|class\s+|typename\s+)*"
    r"[A-Za-z_][A-Za-z0-9_:<>,\s]*?[\s*&]\s*"
    r"(" + "|".join(sorted(SHADOWED)) + r")\s*(?==[^=]|;|\)|,)"
)

# Things that look like a declaration but are a member access or a qualified id.
ACCESS_RE = re.compile(r"(?:->|\.|::)\s*(?:" + "|".join(sorted(SHADOWED)) + r")\b")


def strip_noise(line, in_block):
    out, i, n = [], 0, len(line)
    while i < n:
        if in_block:
            e = line.find("*/", i)
            if e == -1:
                return "".join(out), True
            in_block = False
            i = e + 2
            continue
        c = line[i]
        if c == "/" and i + 1 < n and line[i + 1] == "/":
            break
        if c == "/" and i + 1 < n and line[i + 1] == "*":
            in_block = True
            i += 2
            continue
        if c in "\"'":
            q = c
            i += 1
            while i < n:
                if line[i] == "\\":
                    i += 2
                    continue
                if line[i] == q:
                    i += 1
                    break
                i += 1
            out.append(" ")
            continue
        out.append(c)
        i += 1
    return "".join(out), in_block


# `AFoo::Bar(` — a member function of an AActor-derived class, by Unreal's naming
# convention. Only these scopes can hide AActor's members.
# ANCHORED AT COLUMN 0 AND NOT A STATEMENT. A definition is unindented; a static
# CALL like `ATraceCore::Get(World)` sits inside a line and ends in ';'. Without
# both tests this matched call sites and dragged whole files into "actor scope",
# which produced a false positive on a free function in a component file.
ACTOR_SCOPE_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_:<>,\s\*&]*?\b(A[A-Z][A-Za-z0-9_]*)\s*::\s*[~A-Za-z_][A-Za-z0-9_]*\s*\(")
# A class declaration in a header, so a member function body inside it counts too.
ACTOR_CLASS_RE = re.compile(r"\bclass\s+(?:[A-Z_]+_API\s+)?(A[A-Z][A-Za-z0-9_]*)\s*:")
ANY_CLASS_RE = re.compile(r"\bclass\s+(?:[A-Z_]+_API\s+)?([A-Za-z_][A-Za-z0-9_]*)\s*[:{]")


def scan(path):
    """Report only declarations that sit inside AActor-derived scope."""
    hits = []
    in_block = False
    in_actor_class = False      # header: inside `class AFoo : ...`
    class_depth = 0
    depth = 0
    in_actor_func = False
    func_depth = 0
    func_body_entered = False
    class_body_entered = False

    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for num, raw in enumerate(f, 1):
            code, in_block = strip_noise(raw, in_block)
            if not code.strip():
                depth += code.count("{") - code.count("}")
                continue

            # Entering an AActor-derived class declaration (headers).
            cm = ANY_CLASS_RE.search(code)
            if cm and class_depth == 0:
                in_actor_class = bool(ACTOR_CLASS_RE.search(code))
                class_depth = depth + 1
                class_body_entered = False

            # Entering an AFoo::Bar() definition (.cpp).
            fm = ACTOR_SCOPE_RE.search(code) if ";" not in code else None
            if fm and not in_actor_func:
                in_actor_func = True
                func_depth = depth + 1
                func_body_entered = False

            in_scope = in_actor_class or in_actor_func
            if in_scope:
                probe = ACCESS_RE.sub(" ", code)
                m = DECL_RE.search(probe)
                if m:
                    hits.append((num, m.group(1), raw.strip()[:100]))

            depth += code.count("{") - code.count("}")

            # Only leave the scope once we have actually ENTERED its body. This
            # project writes the brace on the line AFTER the signature, so a
            # naive `depth < func_depth` test fires before the body opens and
            # every actor method looks empty — which is exactly how the first
            # version of this check reported "clean" on the very line MSVC had
            # just rejected.
            if in_actor_func:
                if depth >= func_depth:
                    func_body_entered = True
                elif func_body_entered:
                    in_actor_func = False
                    func_body_entered = False
            if class_depth:
                if depth >= class_depth:
                    class_body_entered = True
                elif class_body_entered:
                    class_depth = 0
                    in_actor_class = False
                    class_body_entered = False
    return hits


def main():
    if not os.path.isdir(SOURCE):
        print("check-engine-member-shadowing: no Source/ directory; nothing to do.")
        return 0

    files = 0
    findings = []
    for dirpath, _d, filenames in os.walk(SOURCE):
        for name in filenames:
            if not name.endswith((".cpp", ".h", ".inl")):
                continue
            path = os.path.join(dirpath, name)
            files += 1
            for num, ident, text in scan(path):
                findings.append((os.path.relpath(path, ROOT), num, ident, text))

    if not findings:
        print("check-engine-member-shadowing: clean ({0} files scanned).".format(files))
        return 0

    print("check-engine-member-shadowing: FAILED — {0} local(s) shadowing an engine "
          "member.".format(len(findings)))
    print()
    print("MSVC makes these ERRORS (C4458). Apple clang cannot warn about them at all —")
    print("UBT hard-disables shadow warnings for clang 17-18.1.3 and Apple clang is 17.0.0 —")
    print("so this compiles here and stops every Windows developer.")
    print()
    for rel, num, ident, text in findings:
        print("  {0}:{1}: local '{2}' shadows the engine member".format(rel, num, ident))
        print("      {0}".format(text))
    print()
    print("FIX: rename the local. 'Owner' -> 'OwningController', 'Instigator' -> 'Attacker',")
    print("and so on. Renaming the local is always correct; the member is not yours to move.")
    return 1


sys.exit(main())
