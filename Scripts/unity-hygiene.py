#!/usr/bin/env python3
# ==============================================================================
# Trace - unity-hygiene.py
#
# ONE RULE: no `using namespace` at file scope in Source/Trace.
#
# WHY IT EXISTS. Unreal compiles this module as UNITY builds - many .cpp files
# concatenated into one translation unit. A using-directive at namespace scope
# applies to the REST OF THAT TU, so it does not stop at the end of the file
# that wrote it: every name the used namespace exports becomes visible,
# unqualified, in every file compiled after it in the same blob.
#
# That broke the Windows build. TraceLoadoutSelect.cpp had
#
#     namespace { using namespace TraceLoadoutLayout; ... }
#
# which put twenty-nine names - Margin, Cyan, Ink, Good, Columns, Rows, Plate,
# RuleY - loose in the TU. TraceMenuHUD.cpp, compiled later in the same blob,
# declared its own local RuleY and Plate, and MSVC raised
#
#     error C4459: declaration of 'RuleY' hides global declaration
#
# which is an ERROR under Unreal's warnings-as-errors.
#
# AND MACOS CANNOT SEE IT. Trace.Build.cs sets ShadowVariableWarningLevel to
# Error and carries a long comment explaining that the setting is a NO-OP on
# this toolchain: UBT forces shadow warnings off for any clang in
# [17, 18.1.3), and Apple clang is 17.0.0. So a clean Mac build proves nothing
# about this class of mistake, and it has now cost two Windows builds - once as
# C4458 (a member named Slot), once as C4459 (this).
#
# This check is the part of that gap a Mac CAN close: it is mechanical, it needs
# no compiler, and it runs in the pre-commit hook on both machines.
#
# THE FIX WHEN IT FIRES is always the same - move the directive inside the
# function that needs it, where it cannot escape, or qualify the uses. Both
# sibling screens already do this: see TraceCharacterSelectFile and
# TraceTeamSelectFile, which are NAMED namespaces for exactly this reason.
#
#   python3 Scripts/unity-hygiene.py --check
# ==============================================================================

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE = os.path.join(ROOT, "Source", "Trace")

# A using-directive with NO leading whitespace is at file scope. One that is
# indented is inside a function or a namespace block - the indented case inside a
# namespace still leaks, so the check below looks at brace depth rather than
# trusting the indentation alone.
USING_RE = re.compile(r"^\s*using\s+namespace\s+([A-Za-z_][A-Za-z0-9_:]*)\s*;")

# `namespace`, `namespace Foo`, `namespace Foo::Bar` - with or without its brace.
NAMESPACE_RE = re.compile(r"^\s*namespace\b[^{;]*$|^\s*namespace\b[^{;]*\{")


def scan(path):
    """
    Returns [(line_no, text)] for every using-directive that ESCAPES its file.

    The discriminator is what each open brace belongs to. A using-directive leaks
    when every enclosing brace is a NAMESPACE brace (including none at all, which
    is file scope); one inside a function body is scoped to that body and is
    fine. Getting this right is the whole value of the check: an earlier version
    keyed on brace DEPTH and reported forty directives, nearly all of them safe,
    which is a check nobody would keep.
    """
    out = []
    stack = []            # one entry per open brace: True if it opened a namespace
    in_block_comment = False
    pending_namespace = False

    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        for line_no, raw in enumerate(handle, 1):
            line = raw.rstrip("\n")

            # Cheap comment/string stripping. This does not need to be a C++ parser;
            # it only has to get brace bookkeeping right.
            work = line
            if in_block_comment:
                end_at = work.find("*/")
                if end_at == -1:
                    continue
                work = work[end_at + 2:]
                in_block_comment = False

            at = work.find("/*")
            while at != -1:
                end_at = work.find("*/", at + 2)
                if end_at == -1:
                    work = work[:at]
                    in_block_comment = True
                    break
                work = work[:at] + work[end_at + 2:]
                at = work.find("/*")

            slashes = work.find("//")
            if slashes != -1:
                work = work[:slashes]

            work = re.sub(r'"(\\.|[^"\\])*"', '""', work)
            work = re.sub(r"'(\\.|[^'\\])*'", "''", work)

            match = USING_RE.match(line)
            if match and all(stack):
                out.append((line_no, line.strip()))

            # `namespace Foo` may put its brace on this line or the next.
            if NAMESPACE_RE.match(work):
                pending_namespace = True

            for char in work:
                if char == "{":
                    stack.append(pending_namespace)
                    pending_namespace = False
                elif char == "}":
                    if stack:
                        stack.pop()

            if "{" in work:
                pending_namespace = False

    return out


BASELINE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "unity-hygiene-baseline.txt")


def load_baseline():
    """
    The directives that were already here when this check was written.

    A BASELINE RATHER THAN A BIG REFACTOR. Seven files in this module already have
    file-scope using-directives and they compile on Windows today - the names they
    export simply do not happen to collide with anything later in their unity
    blob. They are latent, not broken, and rewriting seven unrelated files to
    introduce a lint is how a lint gets reverted. So they are listed, and the
    check fails only on directives added AFTER this point.

    Anything on this list is still a hazard: the day a neighbouring file declares
    a local with a matching name, that blob stops compiling on Windows and the
    error names a file nobody touched. Shortening this list is always welcome.
    """
    if not os.path.exists(BASELINE):
        return set()
    with open(BASELINE, "r", encoding="utf-8") as handle:
        return {
            line.strip()
            for line in handle
            if line.strip() and not line.startswith("#")
        }


def main():
    write_baseline = "--write-baseline" in sys.argv

    found = []
    for dirpath, _dirnames, filenames in os.walk(SOURCE):
        for name in sorted(filenames):
            if not name.endswith((".cpp", ".h", ".inl")):
                continue
            path = os.path.join(dirpath, name)
            for line_no, text in scan(path):
                rel = os.path.relpath(path, ROOT)
                # Keyed on FILE plus the directive, not the line number: a directive
                # that merely moved down the file is the same one, and a baseline
                # that churns on every unrelated edit is a baseline nobody trusts.
                found.append("{0}: {1}".format(rel, text))

    if write_baseline:
        with open(BASELINE, "w", encoding="utf-8") as handle:
            handle.write("# Trace - unity-hygiene baseline.\n")
            handle.write("#\n")
            handle.write("# File-scope `using namespace` directives that predate this check. Each one\n")
            handle.write("# leaks its namespace into every file compiled after it in the same unity\n")
            handle.write("# blob; none of them collides TODAY, which is the only reason Windows builds.\n")
            handle.write("# Regenerate with: python3 Scripts/unity-hygiene.py --write-baseline\n")
            handle.write("#\n")
            for line in sorted(set(found)):
                handle.write(line + "\n")
        print("unity-hygiene: baseline written with {0} entr(ies).".format(len(set(found))))
        return 0

    baseline = load_baseline()
    new_ones = [line for line in found if line not in baseline]

    if not new_ones:
        print("unity-hygiene: clean ({0} baselined, 0 new).".format(len(baseline)))
        return 0

    print("unity-hygiene: FAILED - {0} NEW file-scope `using namespace` directive(s).".format(len(new_ones)))
    print("")
    print("Unity builds concatenate .cpp files, so a using-directive at namespace scope")
    print("leaks every name it imports into the files compiled AFTER it. MSVC then raises")
    print("C4459 'hides global declaration' as an ERROR; clang on macOS says nothing, so a")
    print("clean Mac build is not evidence. This has broken the Windows build twice.")
    print("")
    for line in new_ones:
        print("    " + line)
    print("")
    print("Fix: move the directive INSIDE the function that needs it, or qualify the uses.")
    print("Both sibling select screens keep their file-locals in a NAMED namespace")
    print("(TraceCharacterSelectFile, TraceTeamSelectFile) for exactly this reason.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
