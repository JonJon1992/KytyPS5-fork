#!/usr/bin/env python3
"""Counts the shapes of runtime descriptors from KYTY_RUNTIME_DESCRIPTOR_REPORT=1 log lines.

Usage: descriptor_report.py <log file>...

Each unresolved descriptor is one KYTY_RUNTIME_DESCRIPTOR line (see ResourceTracking.cpp). The
script groups shaders (stage + hash, counted once) by the resource kind, where the table comes
from, what the key depends on and which offset operation is present. It is the Phase 0 tool of
docs/DESCRITORES-DINAMICOS-PLANO.md: run it on the log of a game run with the report enabled.
"""
import collections
import re
import sys

LINE = re.compile(
    r"KYTY_RUNTIME_DESCRIPTOR: stage=(\S+) hash=(0x[0-9a-f]+) kind=(\w+) pc=(0x[0-9a-f]+) "
    r"dword=(\d+) tree=(.*)$"
)


def table_origin(tree):
    if "GetSrtResource" in tree:
        return "srt"
    if "ReadConstBuffer(GetBufferResource" in tree:
        return "buffer V#"
    if "GetUserData" in tree:
        return "user data"
    return "other"


def key_origin(tree):
    if "Phi(" in tree:
        return "loop phi"
    if "FindILsb32" in tree:
        return "bitscan"
    if "ReadFirstLane" in tree or "ReadLane" in tree:
        return "lane/uniform"
    if "GetBuiltin" in tree:
        return "builtin"
    if "GetAttribute" in tree:
        return "attribute"
    return "other"


def offset_form(tree):
    if "IMul32" in tree:
        return "mul stride"
    if "ShiftLeftLogical32" in tree:
        return "shift stride"
    return "none"


def main(paths):
    shaders = collections.defaultdict(set)
    shapes = collections.Counter()
    total = 0
    for path in paths:
        with open(path, errors="replace") as log:
            for text in log:
                match = LINE.search(text)
                if match is None:
                    continue
                stage, shader, kind, _pc, _dword, tree = match.groups()
                shape = (kind, table_origin(tree), key_origin(tree), offset_form(tree))
                shapes[shape] += 1
                shaders[shape].add((stage, shader))
                total += 1
    all_shaders = set().union(*shaders.values()) if shaders else set()
    print(f"{total} reports, {len(all_shaders)} distinct shaders")
    print(f"{'shaders':>8}  {'reports':>7}  {'kind':<8} {'table':<11} {'key':<14} offset")
    for shape, count in sorted(shapes.items(), key=lambda item: -len(shaders[item[0]])):
        kind, table, key, offset = shape
        print(f"{len(shaders[shape]):>8}  {count:>7}  {kind:<8} {table:<11} {key:<14} {offset}")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    main(sys.argv[1:])
