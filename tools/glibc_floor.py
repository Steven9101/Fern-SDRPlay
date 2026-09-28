#!/usr/bin/env python3
# Fern-SDRPlay, an SDRplay RSP input module for FernSDR.
# SPDX-License-Identifier: GPL-2.0-or-later WITH AdditionRef-Fern-SDRPlay-API-exception
"""Report what a module executable needs from the system it runs on.

Prints the shared libraries it names, the newest glibc and libstdc++ symbol
versions it uses, and fails when it names SDRplay's library, which the
module must load at run time only, or needs a glibc newer than --max-glibc.
"""

import argparse
import re
import subprocess
import sys


def version_key(text):
    return tuple(int(part) for part in text.split("."))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable")
    parser.add_argument("--max-glibc", default=None, help="fail when a newer GLIBC_ version is needed")
    args = parser.parse_args()
    dynamic = subprocess.run(["readelf", "-d", args.executable], stdout=subprocess.PIPE, check=True,
                             universal_newlines=True).stdout
    needed = re.findall(r"\(NEEDED\)\s+Shared library: \[([^\]]+)\]", dynamic)
    symbols = subprocess.run(["readelf", "--dyn-syms", "-W", args.executable], stdout=subprocess.PIPE, check=True,
                             universal_newlines=True).stdout
    glibc = sorted(set(re.findall(r"@GLIBC_([0-9.]+)", symbols)), key=version_key)
    glibcxx = sorted(set(re.findall(r"@GLIBCXX_([0-9.]+)", symbols)), key=version_key)
    print("%s: needs %s; glibc %s, libstdc++ GLIBCXX_%s" % (
        args.executable, ", ".join(needed), glibc[-1] if glibc else "none", glibcxx[-1] if glibcxx else "none"))
    if any("sdrplay" in n for n in needed):
        sys.exit("the executable links SDRplay's library; it must load it at run time only")
    if args.max_glibc and glibc and version_key(glibc[-1]) > version_key(args.max_glibc):
        sys.exit("the executable needs glibc %s, newer than %s" % (glibc[-1], args.max_glibc))


if __name__ == "__main__":
    main()
