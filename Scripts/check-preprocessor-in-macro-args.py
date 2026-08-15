#!/usr/bin/env python3
# =============================================================================
# Trace — check-preprocessor-in-macro-args.py
#
# Fails when a preprocessor directive appears INSIDE a function-like macro's
# argument list, e.g.
#
#     UE_LOG(LogTraceGame, Display, TEXT("%s"),
#     #if !UE_BUILD_SHIPPING
#             Something()
#     #else
#             TEXT("")
#     #endif
#             );
#
# WHY THIS EXISTS
#   The C++ standard says the behaviour of a directive inside a macro argument
#   list is UNDEFINED (C++20 [cpp.replace]/11). Clang quietly does the friendly
#   thing; MSVC refuses:
#
#     warning C5101: use of preprocessor directive in function-like macro
#                    argument list is undefined behavior
#     error C2760: syntax error: ',' was unexpected here; expected ')'
#     error C3553: decltype expects an expression not a type
#     error C2059: syntax error: '#'
#
#   So it compiles on macOS and breaks every Windows developer on the team. That
#   has now happened three times in one week, each time discovered by a
#   collaborator rather than by us, because a green build here proves nothing
#   about MSVC.
#
# THE FIX IS ALWAYS THE SAME
#   Hoist the conditional value into a variable ABOVE the call and pass the
#   variable. The #if then sits between statements, where it is perfectly legal.
#
# This is a text scan, not a C++ parser. It tracks parenthesis depth from the
# opening paren of a known function-like macro and reports any line beginning
# with '#' before the matching close. Strings, char literals and comments are
# skipped so a ')' inside TEXT("...)") cannot end the argument list early.
# =============================================================================
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE = os.path.join(ROOT, "Source")

# Function-like macros whose argument lists must stay directive-free. UE_LOG is
# the one that has bitten us; the rest are the same shape and the same hazard.
MACROS = [
    "UE_LOG", "UE_CLOG", "UE_LOGFMT", "UE_LOGFMT_NSLOC",
    "checkf", "checkfSlow", "verifyf", "ensureMsgf", "ensureAlwaysMsgf",
    "UE_LOG_ONCE", "UE_SUPPRESS", "LOCTEXT", "NSLOCTEXT",
    "static_assert", "check", "ensure", "ensureAlways", "verify",
]
MACRO_RE = re.compile(r"\b(" + "|".join(re.escape(m) for m in MACROS) + r")\s*\(")


def strip_noise(line, in_block_comment):
    """Blank out strings, char literals and comments so their parens do not count."""
    out = []
    i = 0
    n = len(line)
    while i < n:
        if in_block_comment:
            end = line.find("*/", i)
            if end == -1:
                return "".join(out), True
            in_block_comment = False
            i = end + 2
            continue
        c = line[i]
        if c == "/" and i + 1 < n and line[i + 1] == "/":
            break
        if c == "/" and i + 1 < n and line[i + 1] == "*":
            in_block_comment = True
            i += 2
            continue
        if c in "\"'":
            quote = c
            i += 1
            while i < n:
                if line[i] == "\\":
                    i += 2
                    continue
                if line[i] == quote:
                    i += 1
                    break
                i += 1
            out.append(" ")
            continue
        out.append(c)
        i += 1
    return "".join(out), in_block_comment


def scan(path):
    findings = []
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        lines = f.readlines()

    depth = 0                 # paren depth inside a tracked macro call
    macro_name = None
    macro_line = 0
    in_block_comment = False

    for num, raw in enumerate(lines, 1):
        stripped = raw.lstrip()

        # A directive while we are inside a tracked macro's argument list.
        if depth > 0 and stripped.startswith("#"):
            directive = stripped.split()[0] if stripped.split() else "#"
            findings.append((num, macro_name, macro_line, directive))
            # Keep scanning; one call can contain #if/#else/#endif and all three
            # are worth naming so the fix is obviously a hoist, not a deletion.

        code, in_block_comment = strip_noise(raw, in_block_comment)

        pos = 0
        while pos < len(code):
            if depth == 0:
                m = MACRO_RE.search(code, pos)
                if not m:
                    break
                macro_name = m.group(1)
                macro_line = num
                depth = 1
                pos = m.end()
            else:
                ch = code[pos]
                if ch == "(":
                    depth += 1
                elif ch == ")":
                    depth -= 1
                    if depth == 0:
                        macro_name = None
                pos += 1
    return findings


def main():
    if not os.path.isdir(SOURCE):
        print("check-preprocessor-in-macro-args: no Source/ directory; nothing to do.")
        return 0

    total_files = 0
    all_findings = []
    for dirpath, _dirnames, filenames in os.walk(SOURCE):
        for name in filenames:
            if not name.endswith((".cpp", ".h", ".inl")):
                continue
            path = os.path.join(dirpath, name)
            total_files += 1
            for num, macro, macro_line, directive in scan(path):
                all_findings.append((os.path.relpath(path, ROOT), num, macro, macro_line, directive))

    if not all_findings:
        print("check-preprocessor-in-macro-args: clean ({0} files scanned).".format(total_files))
        return 0

    print("check-preprocessor-in-macro-args: FAILED — {0} directive(s) inside macro "
          "argument lists.".format(len(all_findings)))
    print()
    print("MSVC rejects these (C5101, then C2760/C3553/C2059). Clang accepts them, so this")
    print("compiles on macOS and breaks every Windows developer.")
    print()
    for rel, num, macro, macro_line, directive in all_findings:
        print("  {0}:{1}: '{2}' inside {3}(...) opened at line {4}".format(
            rel, num, directive, macro or "macro", macro_line))
    print()
    print("FIX: hoist the conditional value into a variable above the call and pass the")
    print("variable, so the #if sits between statements where it is legal:")
    print()
    print("    #if !UE_BUILD_SHIPPING")
    print("        const TCHAR* const Note = Something();")
    print("    #else")
    print("        const TCHAR* const Note = TEXT(\"\");")
    print("    #endif")
    print("        UE_LOG(LogTraceGame, Display, TEXT(\"%s\"), Note);")
    return 1


sys.exit(main())
