"""Census of the shader disk cache: per source, how many permutations, and which specialization
fields make sibling permutations differ.

Usage: python -I perm_census.py <cache_dir> <hash_offset>
<hash_offset>: byte offset of the guest shader hash in this build's source keys ('S' at offset-5).
"""
import collections
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from decode_spec import layout  # noqa: E402


def main():
    cache, hash_at = sys.argv[1], int(sys.argv[2])
    groups = collections.defaultdict(list)
    for name in os.listdir(cache):
        path = os.path.join(cache, name)
        with open(path, "rb") as f:
            head = f.read(32)
            if len(head) < 32:
                continue
            magic, _, key_size, _, _ = struct.unpack("<IIQQQ", head)
            if magic != 0x4344534B or key_size <= hash_at + 8:
                continue
            key = f.read(key_size)
        if key[hash_at - 5] != ord("S"):
            continue
        groups[key[hash_at:hash_at + 8]].append(key)
    sources = perms = 0
    per_source = collections.Counter()
    field_diffs = collections.Counter()
    kinds = collections.Counter()
    for keys in groups.values():
        keys.sort(key=len)
        srcs = [k for k in keys if not any(len(o) < len(k) and k.startswith(o) for o in keys)]
        for s in srcs:
            sources += 1
            tails = [k[len(s):] for k in keys if len(k) > len(s) and k.startswith(s)]
            perms += len(tails)
            per_source[min(len(tails), 10)] += 1
            if len(tails) < 2:
                continue
            fields, size = layout(tails[0])
            if any(len(t) != size for t in tails):
                kinds["different resource counts"] += 1
                continue
            differing = set()
            for name, at, width in fields:
                if len({t[at:at + width] for t in tails}) > 1:
                    differing.add(name.split(".")[-1] if "[" in name else name)
            for d in differing:
                field_diffs[d] += 1
            kinds["only push_data_cursor" if differing == {"push_data_cursor"} else
                  "only image dimension/numeric_class/cube" if differing and differing <= {"dimension", "numeric_class", "cube"} else
                  "image type + other" if differing & {"dimension", "numeric_class", "cube"} else "other"] += 1
    print(f"sources {sources}, permutations {perms}, perms/source {perms / max(sources, 1):.2f}")
    print("sources by permutation count (10 = 10+):", dict(sorted(per_source.items())))
    print("sources with >1 permutation, what differs:", dict(kinds.most_common()))
    print("field differing among siblings (sources):", dict(field_diffs.most_common(15)))


if __name__ == "__main__":
    main()
