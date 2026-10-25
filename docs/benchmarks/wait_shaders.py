"""B1 input: the SPIR-V of every pipeline a session waited for, pulled from the shader cache.

Reads the [pipeline-wait] lines of the given logs, finds in the cache the permutation of each
guest shader whose word count adds up to the logged total, and writes per pipeline
<n>.vs.spv + <n>.ps.spv (or <n>.cs.spv), plus waits.csv (n, kind, hashes, words, guest wave sizes,
game ms).

Usage: python -I wait_shaders.py <cache_dir> <out_dir> <kyty_log.txt>...
"""
import collections
import csv
import os
import re
import struct
import sys

# Guest hash offset in permutation keys: current SourceVersion, then the one before phase 1.
HASH_OFFSETS = (2155, 2118)
WAIT = re.compile(r"\[pipeline-wait\] at=\S+ (VS|CS) 0x([0-9a-f]{16}) PS 0x([0-9a-f]{16}) words=(\d+)"
                  r" reason=(\S+)( unoptimized)? waited=\d+ ms queued=\d+ ms compiling=(\d+) ms")


def load_cache(cache):
    """guest hash -> {word count: (mtime, spirv bytes, wave size)}, newest record per size."""
    shaders = collections.defaultdict(dict)
    for name in os.listdir(cache):
        path = os.path.join(cache, name)
        with open(path, "rb") as f:
            head = f.read(32)
            if len(head) < 32:
                continue
            magic, _, key_size, payload_size, _ = struct.unpack("<IIQQQ", head)
            if magic != 0x4344534B:
                continue
            key = f.read(key_size)
            at = next((o for o in HASH_OFFSETS if len(key) > o + 8 and key[o - 5] == ord("S")), None)
            if at is None:
                continue
            payload = f.read(payload_size)
        if len(payload) < 28:
            continue
        count = struct.unpack_from("<Q", payload, 0)[0]
        if count < 5 or count * 4 + 8 > len(payload) or struct.unpack_from("<I", payload, 8)[0] != 0x07230203:
            continue
        mtime = os.path.getmtime(path)
        known = shaders[key[at:at + 8]]
        if count not in known or known[count][0] < mtime:
            # Key: 'S', stage u32, guest hash u64, wave size u32.
            known[count] = (mtime, payload[8:8 + count * 4], struct.unpack_from("<I", key, at + 8)[0])
    return shaders


def main():
    cache, out = sys.argv[1], sys.argv[2]
    shaders = load_cache(cache)
    os.makedirs(out, exist_ok=True)
    seen, rows, missing = set(), [], 0
    for log in sys.argv[3:]:
        session = os.path.basename(os.path.dirname(os.path.abspath(log)))
        with open(log, encoding="utf-8", errors="replace") as f:
            for line in f:
                m = WAIT.search(line)
                if not m:
                    continue
                kind, vs, ps, words, reason, unopt, ms = m.groups()
                if (kind, vs, ps, words) in seen:
                    continue
                seen.add((kind, vs, ps, words))
                first = shaders.get(int(vs, 16).to_bytes(8, "little"), {})
                total = int(words)
                if kind == "CS":
                    pair = (first[total], None) if total in first else None
                else:
                    second = shaders.get(int(ps, 16).to_bytes(8, "little"), {})
                    pair = next(((first[a], second[total - a]) for a in first if total - a in second), None)
                if pair is None:
                    missing += 1
                    continue
                n = len(rows)
                if kind == "CS":
                    open(os.path.join(out, f"{n}.cs.spv"), "wb").write(pair[0][1])
                else:
                    open(os.path.join(out, f"{n}.vs.spv"), "wb").write(pair[0][1])
                    open(os.path.join(out, f"{n}.ps.spv"), "wb").write(pair[1][1])
                waves = [pair[0][2], pair[1][2] if pair[1] else 0]
                rows.append([n, kind, vs, ps, total, *waves, reason, "unoptimized" if unopt else "optimized", ms,
                             session])
    with open(os.path.join(out, "waits.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["n", "kind", "vs", "ps", "words", "wave", "ps_wave", "reason", "mode", "game_ms", "session"])
        w.writerows(rows)
    print(f"{len(rows)} pipelines written to {out}; {missing} waits had no matching cache records")


if __name__ == "__main__":
    main()
