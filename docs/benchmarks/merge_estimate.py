"""Quantas permutações sumiriam com KYTY_ARRAYED_IMAGES (fase 1 de docs/AUDITORIA-SHADERS-PIPELINES.md).

Uso: python -I merge_estimate.py <cache_dir> <hash_offset>   (mesmos argumentos do perm_census.py)

Com a chave ligada, uma instrução de array mantém o tipo array numa textura de uma camada: as dimensões
2D (3) e 2D array (5) de um slot sem cube viram 5, 1D (1) / 1D array (2) viram 2 e MSAA (6) / MSAA array (7)
viram 7. Não distingue imagem amostrada de storage (a chave fica só nas amostradas). Conta as permutações
de cada shader antes e depois de juntar essas dimensões.
"""
import collections
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from decode_spec import layout  # noqa: E402

ARRAYED = {3: 5, 1: 2, 6: 7}  # Dim2D -> Dim2DArray, Dim1D -> Dim1DArray, Dim2DMsaa -> Dim2DMsaaArray


def merged_tails(tails):
    fields, _ = layout(tails[0])
    dims = [(name, at) for name, at, _ in fields if name.endswith(".dimension")]
    cubes = {name.rsplit(".", 1)[0]: at for name, at, _ in fields if name.endswith(".cube")}
    out = [bytearray(t) for t in tails]
    for name, at in dims:
        values = {struct.unpack_from("<I", t, at)[0] for t in tails}
        slot_cube = {t[cubes[name.rsplit(".", 1)[0]]] for t in tails}
        # Só um slot onde o tipo array já aparece prova que a instrução pede array.
        if slot_cube == {0} and any(v in ARRAYED.values() for v in values):
            for t in out:
                v = struct.unpack_from("<I", t, at)[0]
                struct.pack_into("<I", t, at, ARRAYED.get(v, v))
    return {bytes(t) for t in out}


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
        if key[hash_at - 5] == ord("S"):
            groups[key[hash_at:hash_at + 8]].append(key)
    before = after = sources_merged = 0
    for keys in groups.values():
        keys.sort(key=len)
        for s in [k for k in keys if not any(len(o) < len(k) and k.startswith(o) for o in keys)]:
            tails = [k[len(s):] for k in keys if len(k) > len(s) and k.startswith(s)]
            if not tails:
                continue
            before += len(tails)
            sizes = {len(t) for t in tails}
            merged = len(merged_tails(tails)) if len(tails) > 1 and len(sizes) == 1 else len(tails)
            after += merged
            sources_merged += merged < len(tails)
    print(f"permutações {before} -> {after} (-{before - after}); shaders com permutações juntadas: {sources_merged}")


if __name__ == "__main__":
    main()
