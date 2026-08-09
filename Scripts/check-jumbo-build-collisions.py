#!/usr/bin/env python3
"""
Find symbols defined in more than one anonymous namespace in the Trace module.

NOTE ON THE NAME: a "unity build" (a.k.a. jumbo build) is a C++ compilation technique -- the
build system concatenates many .cpp files into one before compiling, because one big file
compiles faster than many small ones. It has NOTHING to do with the Unity game engine.
Unreal Build Tool does this by default.

Why this exists: UBT compiles the module as a unity/jumbo build, concatenating many .cpp files
into one translation unit. Two files that each define `namespace { void Foo(); }` are perfectly
legal C++ on their own, but once concatenated they are one namespace with two definitions of
Foo -- MSVC C2084, "function already has a body".

Which files get grouped together depends on file count and ordering, so the same source can
build clean on macOS and fail on Windows. That has broken the Windows build once already
(TraceAbilityVerify / TraceXVerify / TraceCharacterVerify / TraceMaceOysterVerify, all four
defining FindAuthoritativeWorld).

Fix a hit by naming the namespace after its file (`namespace TraceFooVerify { ... }`), which is
what the verify files now do -- not by renaming the individual symbol, which only defers the
problem to the next collision.

Exit code 1 if any collision is found, so build.sh can gate on it.
"""

import os
import re
import sys
from collections import defaultdict

MODULE_ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "Source", "Trace")

# A definition at one-tab depth inside an anonymous namespace. We deliberately keep this
# conservative: false negatives are acceptable (the compiler is the real gate), false
# positives are not, because they would block builds on legal code.
FUNC = re.compile(r"^\t(?:template\s*<[^>]*>\s*)?(?:[A-Za-z_][\w:<>,\s\*&\[\]]*?[\s\*&])?([A-Za-z_]\w*)\s*\(")
VAR = re.compile(r"^\t(?:static\s+|const\s+|constexpr\s+|inline\s+)*[A-Za-z_][\w:<>,\s\*&]*?[\s\*&]([A-Za-z_]\w*)\s*(?:=|;|\[)")
TYPE = re.compile(r"^\t(?:struct|class|enum(?:\s+class)?|union)\s+([A-Za-z_]\w*)")

# Keywords that the regexes above can mistake for a definition name.
NOISE = {
    "if", "for", "while", "switch", "return", "else", "do", "case", "sizeof",
    "static_assert", "using", "typedef", "friend", "operator", "namespace",
}


def strip_comments_and_strings(text):
    """Blank out block comments, line comments and string literals so they cannot match."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join("\n" if ch == "\n" else " " for ch in text[i:j]))
            i = j
        elif c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif c in "\"'":
            quote, j = c, i + 1
            while j < n and text[j] != quote:
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            out.append(" " * (j - i))
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def anon_namespace_symbols(path):
    """Yield names defined directly inside an anonymous namespace in this file."""
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        raw = handle.read()
    clean = strip_comments_and_strings(raw).split("\n")

    symbols = {}
    depth = None  # brace depth relative to the anonymous namespace, or None when outside one
    for number, line in enumerate(clean, start=1):
        if depth is None:
            # An anonymous namespace opens at column 0 as a bare `namespace`, with the brace
            # on the next line (this module's style) or on the same line.
            if re.match(r"^namespace\s*\{?\s*$", line):
                depth = 0 if "{" in line else -1  # -1 == waiting for the opening brace
            continue

        if depth == -1:
            if "{" in line:
                depth = line.count("{") - line.count("}")
            continue

        if depth == 1:
            for pattern in (TYPE, FUNC, VAR):
                match = pattern.match(line)
                if match and match.group(1) not in NOISE:
                    symbols.setdefault(match.group(1), number)
                    break

        depth += line.count("{") - line.count("}")
        if depth <= 0:
            depth = None

    return symbols


def main():
    owners = defaultdict(list)
    for root, _dirs, files in os.walk(MODULE_ROOT):
        for name in sorted(files):
            if not name.endswith(".cpp"):
                continue
            path = os.path.join(root, name)
            rel = os.path.relpath(path, os.path.join(MODULE_ROOT, "..", ".."))
            for symbol, line in anon_namespace_symbols(path).items():
                owners[symbol].append((rel, line))

    collisions = {s: w for s, w in owners.items() if len(w) > 1}
    if not collisions:
        print("check-jumbo-build-collisions: clean (%d anonymous-namespace symbols, no duplicates)"
              % len(owners))
        return 0

    print("check-jumbo-build-collisions: %d symbol(s) defined in more than one anonymous namespace."
          % len(collisions))
    print("Under a unity/jumbo build these become redefinitions (MSVC C2084). Name the namespace")
    print("after its file to fix a whole file at once.\n")
    for symbol in sorted(collisions):
        print("  %s" % symbol)
        for rel, line in sorted(collisions[symbol]):
            print("      %s:%d" % (rel, line))
    return 1


if __name__ == "__main__":
    sys.exit(main())
