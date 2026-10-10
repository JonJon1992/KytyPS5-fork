#!/usr/bin/env python3
"""Inspect the bounded GPU BVH witnesses emitted by KYTY_BVH_CAPTURE_SHADER.

A witness contains executed instruction inputs/results, not a full scene or
proof that hardware traversal can replace the guest shader.
"""
import argparse
from collections import Counter, defaultdict
import hashlib
import json
import math
from pathlib import Path
import struct
import sys

MAGIC, VERSION = 0x43564842, 2
HEADER_WORDS, RECORD_WORDS, MAX_RECORDS = 64, 64, 65536


def read_capture(path):
    size = path.stat().st_size
    if size < HEADER_WORDS * 4 or size > (HEADER_WORDS + MAX_RECORDS * RECORD_WORDS) * 4:
        raise ValueError("file size is outside the bounded capture format")
    data = path.read_bytes()
    header = struct.unpack_from("<64I", data)
    count, enabled, magic, version, stride, capacity = header[:6]
    if magic != MAGIC or version not in (1, VERSION) or stride != RECORD_WORDS:
        raise ValueError("unsupported magic, version or record stride")
    if enabled != 1 or not 0 < capacity <= MAX_RECORDS or count > capacity:
        raise ValueError("invalid capture control fields or count")
    if len(data) != (HEADER_WORDS + count * RECORD_WORDS) * 4:
        raise ValueError("record count does not match the exact file size")
    if header[14] not in (0, 1):
        raise ValueError("invalid direct/indirect dispatch kind")
    records = []
    for i in range(count):
        record = struct.unpack_from("<64I", data, (HEADER_WORDS + i * RECORD_WORDS) * 4)
        kind = record[1] & 7
        status = record[57] if version >= 2 else 1
        if status not in (1, 2, 3):
            raise ValueError(f"record {i} has an invalid guard status")
        if status == 1 and (kind > 5 or record[3] != (32 if kind == 5 else 16)):
            raise ValueError(f"record {i} has an invalid node kind/size")
        if status != 1 and record[3] != 0:
            raise ValueError(f"record {i} contains bytes for a rejected node")
        records.append(record)
    return header, records, hashlib.sha256(data).hexdigest()


def floats(words):
    return struct.unpack("<" + "f" * len(words), struct.pack("<" + "I" * len(words), *words))


def summarize(path):
    header, records, digest = read_capture(path)
    pcs = Counter()
    kinds = Counter()
    descriptors = Counter()
    statuses = Counter()
    payloads = defaultdict(set)
    finite = 0
    negative_extent = 0
    instruction_hits = 0
    hits_within_extent = 0
    for record in records:
        pc, low, high, words = record[:4]
        descriptor = record[4:8]
        node = low | high << 32
        status = record[57] if header[3] >= 2 else 1
        statuses[status] += 1
        kinds[node & 7] += 1
        pcs[pc] += 1
        descriptors[descriptor] += 1
        if status == 1:
            payloads[(descriptor, node)].add(record[22:22 + words])
        extent, *ray = floats(record[8:15])
        finite += all(math.isfinite(value) for value in ray) and math.isfinite(extent)
        negative_extent += extent < 0
        if status == 1 and node & 7 < 4:
            numerator, denominator = floats(record[18:20])
            # The instruction returns a numerator/denominator. The guest shader
            # applies its own distance, facing, table and instance policy later.
            if math.isfinite(numerator) and math.isfinite(denominator) and denominator:
                t = numerator / denominator
                if math.isfinite(t):
                    instruction_hits += 1
                    hits_within_extent += 0 <= t <= extent
    return {
        "file": str(path.resolve()),
        "sha256": digest,
        "shader": f"{header[6] | header[7] << 32:016x}",
        "tick": header[12] | header[13] << 32,
        "dispatch": "indirect" if header[14] else "direct",
        "groups": list(header[8:11]),
        "mode": header[11],
        "records": header[0],
        "capacity": header[5],
        "capacity_reached": header[0] == header[5],
        "sampling_mask": header[15],
        "instruction_pc_filter": f"0x{header[16]:x}" if header[3] >= 2 and header[16] else None,
        "pcs": {f"0x{pc:x}": count for pc, count in sorted(pcs.items())},
        "node_kinds": dict(sorted(kinds.items())),
        "guard_status": {str(key): value for key, value in sorted(statuses.items())},
        "descriptors": [{"words": [f"0x{word:08x}" for word in descriptor], "records": count}
                        for descriptor, count in descriptors.most_common()],
        "unique_descriptor_nodes": len(payloads),
        "nodes_with_different_payloads": sum(len(values) != 1 for values in payloads.values()),
        "finite_origin_direction_extent": finite,
        "negative_extent": negative_extent,
        "finite_triangle_instruction_hits": instruction_hits,
        "triangle_hits_in_nonnegative_extent": hits_within_extent,
        "scope": "executed BVH instruction witnesses; not a complete traversal snapshot",
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("--output", type=Path, help="write the JSON report to this file")
    args = parser.parse_args()
    try:
        report = summarize(args.capture)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    text = json.dumps(report, indent=2) + "\n"
    if args.output:
        args.output.write_text(text)
    sys.stdout.write(text)


if __name__ == "__main__":
    main()
