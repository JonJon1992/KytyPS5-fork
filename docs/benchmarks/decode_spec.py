"""Names the ResourceSpecialization field at each differing tail offset of a guest shader's permutations.

Usage: python -I decode_spec.py <cache_dir> <hash_hex> <since MM-DD>
Tail = 'P' + vector<Buffer> + vector<Image> + vector<Sampler> + u32 push_data_cursor
(shaderDiskCache.cpp Writer: vectors as u64 count, enums as u32, bools as 1 byte).
"""
import datetime
import os
import struct
import sys

BUFFER = [("packed_stride", 4), ("descriptor_format", 4), ("descriptor_swizzle", 4), ("zero_stride_oob", 1),
          ("write_tracked", 1), ("indirect_root", 4), ("indirect_mapping_offset", 4),
          ("indirect_search_iterations", 4), ("gpu_records_flat", 4), ("gpu_records_add_flat", 4),
          ("gpu_records_add", 4), ("gpu_records_ssa", 1)]
IMAGE = [("numeric_class", 4), ("dimension", 4), ("mip_count", 4), ("conversion_format", 4),
         ("shader_swizzle", 4), ("indirect_root", 4), ("indirect_mapping_offset", 4),
         ("indirect_search_iterations", 4), ("cube", 1), ("fmask", 1), ("bindless", 1)]
SAMPLER = [("bindless", 1), ("bindless_mapping_offset", 4)]


def layout(tail):
    """[(offset, size, name)] for every field of the tail."""
    fields, at = [("P", 0, 1)], 1
    for kind, members in (("buffer", BUFFER), ("image", IMAGE), ("sampler", SAMPLER)):
        count = struct.unpack_from("<Q", tail, at)[0]
        fields.append((f"{kind}s.count", at, 8))
        at += 8
        for i in range(count):
            for name, size in members:
                fields.append((f"{kind}[{i}].{name}", at, size))
                at += size
    fields.append(("push_data_cursor", at, 4))
    return fields, at + 4


def records(cache, target):
    out = []
    for name in os.listdir(cache):
        path = os.path.join(cache, name)
        with open(path, "rb") as f:
            head = f.read(32)
            if len(head) < 32:
                continue
            magic, _, key_size, _, _ = struct.unpack("<IIQQQ", head)
            if magic != 0x4344534B:
                continue
            key = f.read(key_size)
        at = key[:8192].find(target)
        if at >= 5 and key[at - 5] == ord("S"):
            out.append((datetime.datetime.fromtimestamp(os.path.getmtime(path)).strftime("%m-%d %H:%M:%S"), key))
    return sorted(out)


def main():
    cache, target, since = sys.argv[1], int(sys.argv[2], 16).to_bytes(8, "little"), sys.argv[3]
    recs = records(cache, target)
    sources = [r for r in recs if not any(len(o[1]) < len(r[1]) and r[1].startswith(o[1]) for o in recs)]
    source = sources[-1][1]
    perms = [(w, k[len(source):]) for w, k in recs if len(k) > len(source) and k.startswith(source) and w >= since]
    fields, size = layout(perms[0][1])
    print(f"tail {len(perms[0][1])} B, layout covers {size} B")
    for name, at, width in fields:
        values = [t[at:at + width] for _, t in perms]
        if len(set(values)) > 1:
            shown = "  ".join(f"{w[6:]}={int.from_bytes(v, 'little')}" for (w, _), v in zip(perms, values))
            print(f"  {name} @{at}: {shown}")


if __name__ == "__main__":
    main()
