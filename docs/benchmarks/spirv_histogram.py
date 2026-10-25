"""Opcode histogram of the SPIR-V in a guest shader's newest permutation record.

Usage: python -I spirv_histogram.py <cache_dir> <hash_hex>
Permutation payload = PutWords(spirv) (u64 word count + words) + compiled shader info.
"""
import collections
import os
import struct
import sys

NAMES = {1: "Undef", 15: "EntryPoint", 20: "TypeBool", 21: "TypeInt", 22: "TypeFloat", 23: "TypeVector",
         32: "TypePointer", 43: "Constant", 54: "Function", 59: "Variable", 61: "Load", 62: "Store",
         65: "AccessChain", 71: "Decorate", 79: "VectorShuffle", 80: "CompositeConstruct",
         81: "CompositeExtract", 124: "Bitcast", 128: "IAdd", 129: "FAdd", 130: "ISub", 132: "IMul",
         133: "FMul", 136: "FDiv", 141: "FMod", 169: "Select", 170: "IEqual", 171: "INotEqual",
         172: "UGreaterThan", 176: "ULessThan", 180: "FOrdEqual", 182: "FOrdNotEqual", 184: "FOrdLessThan",
         186: "FOrdGreaterThan", 194: "ShiftRightLogical", 195: "ShiftRightArithmetic", 196: "ShiftLeftLogical",
         197: "BitwiseOr", 198: "BitwiseXor", 199: "BitwiseAnd", 201: "BitFieldInsert",
         202: "BitFieldSExtract", 203: "BitFieldUExtract", 245: "Phi", 246: "LoopMerge",
         247: "SelectionMerge", 248: "Label", 249: "Branch", 250: "BranchConditional", 251: "Switch",
         252: "Kill", 253: "Return", 12: "ExtInst", 87: "ImageSampleImplicitLod", 88: "ImageSampleExplicitLod",
         95: "ImageFetch", 98: "ImageRead", 86: "SampledImage", 100: "Image", 57: "FunctionCall",
         164: "LogicalEqual", 166: "LogicalOr", 167: "LogicalAnd", 168: "LogicalNot", 112: "ConvertUToF",
         110: "ConvertFToS", 109: "ConvertFToU", 111: "ConvertSToF", 113: "UConvert", 115: "FConvert"}


def main():
    cache, target = sys.argv[1], int(sys.argv[2], 16).to_bytes(8, "little")
    best = None
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
            at = key[:8192].find(target)
            if at < 5 or key[at - 5] != ord("S"):
                continue
            payload = f.read(payload_size)
        if len(payload) < 8:
            continue
        count = struct.unpack_from("<Q", payload, 0)[0]
        if count * 4 + 8 > len(payload) or count < 5:
            continue
        words = struct.unpack_from(f"<{count}I", payload, 8)
        if words[0] != 0x07230203:
            continue
        mtime = os.path.getmtime(path)
        if best is None or mtime > best[0]:
            best = (mtime, words)
    words = best[1]
    ops, at, functions, blocks = collections.Counter(), 5, 0, 0
    while at < len(words):
        length, op = words[at] >> 16, words[at] & 0xFFFF
        ops[op] += length
        functions += op == 54
        blocks += op == 248
        at += max(length, 1)
    total = sum(ops.values())
    print(f"SPIR-V {len(words)} words; functions {functions}; basic blocks {blocks}")
    for op, n in ops.most_common(18):
        print(f"  {NAMES.get(op, op):<24} {n:>7} words  {100 * n / total:5.1f}%")


if __name__ == "__main__":
    main()
