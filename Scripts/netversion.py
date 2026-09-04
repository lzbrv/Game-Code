#!/usr/bin/env python3
"""
Print the network compatibility value this checkout will build.

WHAT THIS IS FOR
================
Two machines can only join each other if their network version matches. Getting
that wrong costs an evening, because the failure surfaces as "connection failed"
and names nothing. This script answers "will my build agree with yours?" in one
second, from the SOURCE, on any platform, with no engine and no build:

    python3 Scripts/netversion.py

Run it on the Mac and on the Windows machine. If the two lines are identical,
the two builds of that checkout will agree. If they differ, the checkouts differ
and no amount of rebuilding will fix it.

WHY A SECOND IMPLEMENTATION IS SAFE HERE, WHICH IS A CLAIM THAT NEEDS EVIDENCE
=============================================================================
Two implementations of one rule normally drift, and this project has paid for
that more than once. Three things keep this one honest:

  1. IT READS THE SOURCE, IT DOES NOT COPY IT. NetProtocolVersion is parsed out
     of Source/Trace/UI/TraceNetworking.h and ProjectVersion out of
     Config/DefaultGame.ini. There is no constant here to fall out of date; if
     somebody bumps the protocol, this script says so on the next run without
     being touched. If either read FAILS, the script exits non-zero and says
     which one — it never guesses a default, because a guessed default is
     exactly how you would get two machines printing the same wrong number.

  2. IT PRINTS THE STRING, NOT ONLY THE CHECKSUM. The game prints the same
     string ("trace netproto 1, project 0.1.0") next to its checksum in the log,
     in Trace.NetVersion, and the checksum alone on the title screen. A human can
     see at a glance whether the two agree about the INPUTS, not just whether two
     opaque hex numbers happen to match.

  3. --verify RUNS THE COMPARISON. Point it at a log from a real run of the game
     and it checks this script's answer against the game's own printed value:

         python3 Scripts/netversion.py --verify Saved/Logs/Trace.log

     That closes the loop: the reimplementation below is not trusted, it is
     tested against the binary that actually does the handshake.

THE CHECKSUM
============
The engine hashes with FCrc::StrCrc32 (Engine/Source/Runtime/Core/Public/Misc/
Crc.h). That is a standard reflected CRC-32 (poly 0xEDB88320 — the table in
Crc.cpp:211 is the textbook one) with one wrinkle worth stating, because it is
also the reason the value is portable at all: StrCrc32 widens EVERY CHARACTER TO
32 BITS before folding it in, deliberately, so that a string hashes to the same
number whether TCHAR is 2 bytes (Windows) or 4 (macOS). Its own comment says so.
So the Python equivalent is CRC-32 over the string encoded as UTF-32 little
endian, which is what crc32_utf32le() below does.
"""

import argparse
import os
import re
import sys
import zlib

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

HEADER = os.path.join(REPO, "Source", "Trace", "UI", "TraceNetworking.h")
GAME_INI = os.path.join(REPO, "Config", "DefaultGame.ini")

# Matches:  inline constexpr int32 NetProtocolVersion = 1;
PROTO_RE = re.compile(
    r"^\s*inline\s+constexpr\s+int32\s+NetProtocolVersion\s*=\s*(-?\d+)\s*;", re.MULTILINE
)

# The game's own printed form, for --verify. Both of these appear in a Development log:
#   [Net] compatibility value pinned by the project: NET 1A2B3C4D  ("trace netproto 1, project 0.1.0")
#   [Net] NET 1A2B3C4D  ("trace netproto 1, project 0.1.0")  protocol=1  override=installed
LOG_RE = re.compile(r'NET\s+([0-9A-Fa-f]{8})\b.*?\("([^"]*)"\)')


def die(message):
    print("netversion: " + message, file=sys.stderr)
    raise SystemExit(2)


def crc32_utf32le(text):
    """FCrc::StrCrc32 for an ASCII/BMP string. See the module docstring for why this is equal."""
    return zlib.crc32(text.encode("utf-32-le")) & 0xFFFFFFFF


def read_protocol_version():
    if not os.path.isfile(HEADER):
        die("cannot read %s — is this a Trace checkout?" % HEADER)
    with open(HEADER, "r", encoding="utf-8") as handle:
        text = handle.read()
    match = PROTO_RE.search(text)
    if match is None:
        die(
            "no 'inline constexpr int32 NetProtocolVersion = N;' in %s.\n"
            "            The declaration moved or was renamed. Fix this script rather than\n"
            "            guessing a value: a guess would make two machines agree on a number\n"
            "            neither of their builds actually uses." % HEADER
        )
    return int(match.group(1))


def read_project_version():
    """ProjectVersion out of [/Script/EngineSettings.GeneralProjectSettings].

    Deliberately NOT configparser: DefaultGame.ini uses UE's ini dialect (duplicate
    keys, '+' and '!' prefixes, ';' comments, values containing '='), and configparser
    rejects several of those outright.
    """
    if not os.path.isfile(GAME_INI):
        die("cannot read %s" % GAME_INI)
    section = None
    value = None
    with open(GAME_INI, "r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            stripped = line.strip()
            if not stripped or stripped.startswith(";") or stripped.startswith("#"):
                continue
            if stripped.startswith("[") and stripped.endswith("]"):
                section = stripped[1:-1]
                continue
            if section != "/Script/EngineSettings.GeneralProjectSettings":
                continue
            if "=" not in stripped:
                continue
            key, _, raw = stripped.partition("=")
            if key.strip() == "ProjectVersion":
                # LAST WINS, matching UE: a later line in the same section overrides an
                # earlier one, so this must not break out of the loop on the first hit.
                value = raw.strip()
    if not value:
        # The game substitutes "unset" here rather than an empty string, precisely so that
        # a machine with a broken config still agrees with one whose config is fine. Mirror
        # that instead of failing, so the two implementations agree in the broken case too.
        return "unset"
    return value


def compute():
    protocol = read_protocol_version()
    project = read_project_version()
    string = "trace netproto %d, project %s" % (protocol, project.lower())
    return protocol, project, string, crc32_utf32le(string)


def verify(log_path):
    if not os.path.isfile(log_path):
        die("no such log: %s" % log_path)
    _, _, string, checksum = compute()

    found = []
    with open(log_path, "r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            match = LOG_RE.search(line)
            if match is not None:
                found.append((int(match.group(1), 16), match.group(2)))

    if not found:
        print(
            "VERIFY INCONCLUSIVE: %s contains no '[Net] ... NET xxxxxxxx (\"...\")' line.\n"
            "  A Shipping build cannot produce one (logging is compiled out) — use a\n"
            "  Development build, or read the code off the title screen instead." % log_path
        )
        return 3

    ok = True
    for logged_checksum, logged_string in found:
        agree = (logged_checksum == checksum) and (logged_string == string)
        print(
            "  game said NET %08X  (\"%s\")   %s"
            % (logged_checksum, logged_string, "MATCH" if agree else "*** MISMATCH ***")
        )
        ok = ok and agree

    print("  script says NET %08X  (\"%s\")" % (checksum, string))
    if ok:
        print("VERIFIED: this script and the built game agree, on both the string and the checksum.")
        return 0
    print(
        "FAILED: the script and the game disagree. The script is wrong, or the log is from a\n"
        "        different checkout. Trust the GAME — it is the thing that does the handshake."
    )
    return 1


def main():
    parser = argparse.ArgumentParser(
        description="Print (or verify) this checkout's network compatibility value."
    )
    parser.add_argument(
        "--verify",
        metavar="LOG",
        help="Check this script against a Development run's log instead of just printing.",
    )
    parser.add_argument("--quiet", action="store_true", help="Print only 'NET xxxxxxxx'.")
    args = parser.parse_args()

    if args.verify:
        raise SystemExit(verify(args.verify))

    protocol, project, string, checksum = compute()

    if args.quiet:
        print("NET %08X" % checksum)
        return

    print("NET %08X" % checksum)
    print('  from : "%s"' % string)
    print("  protocol version : %d      (Source/Trace/UI/TraceNetworking.h)" % protocol)
    print("  project version  : %s  (Config/DefaultGame.ini)" % project)
    print("")
    print("Every machine in a session must print this same line. The built game shows the")
    print("same code on its title screen, bottom right, in every configuration including")
    print("Shipping — that is the check to use when you cannot run this script.")


if __name__ == "__main__":
    main()
