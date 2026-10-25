"""Resumo de uma sessão (benchmark B3 de docs/AUDITORIA-SHADERS-PIPELINES.md).

Uso: python -I resumo_rodada.py <pasta>      (fase1, ou uma pasta de fase1\rodadas)
     python -I resumo_rodada.py --self-test

Lê <pasta>/frames.csv (KYTY_FRAME_TIMES) e as linhas [pipeline-wait] / [shader-compile] /
[shader-module] de <pasta>/kyty_log.txt. As duas fontes usam o mesmo relógio (segundos do steady clock), então dá para
dizer quanto de cada travamento foi espera de compilação.
"""
import collections
import os
import re
import sys

STALL_MS = 1000.0  # um quadro que demorou isso ou mais conta como travamento
WAIT_RE = re.compile(r"\[pipeline-wait\](?: at=(?P<at>[\d.]+))? (?P<stage>\w+) 0x(?P<vs>[0-9a-f]+) PS 0x(?P<ps>[0-9a-f]+)"
                     r" words=(?P<words>\d+) reason=(?P<reason>[\w-]+)(?P<unopt> unoptimized)?"
                     r" waited=(?P<waited>\d+) ms queued=(?P<queued>\d+) ms compiling=(?P<compiling>\d+) ms")
COMPILE_RE = re.compile(r"\[shader-compile\] at=(?P<at>[\d.]+) (?P<stage>\S+) 0x(?P<hash>[0-9a-f]+) (?P<ms>\d+) ms"
                        r" words=(?P<words>\d+) translated=(?P<where>\w+)"
                        r"(?: compiled=worker\((?P<worker_ms>\d+) ms\))?")
MODULE_RE = re.compile(r"\[shader-module\] at=(?P<at>[\d.]+) (?P<stage>\S+) 0x(?P<hash>[0-9a-f]+) (?P<ms>\d+) ms")


def percentile(values, p):
    """Nearest-rank percentile of a non-empty list."""
    ordered = sorted(values)
    rank = max(1, -(-len(ordered) * p // 100))  # ceil
    return ordered[int(rank) - 1]


def read_frames(path):
    frames = []
    with open(path, encoding="utf-8", errors="replace") as f:
        next(f, None)
        for line in f:
            parts = line.strip().split(",")
            if len(parts) == 2:
                try:
                    frames.append((float(parts[0]), float(parts[1])))
                except ValueError:
                    pass
    return frames[1:]  # o primeiro quadro não tem anterior


def read_log(path):
    waits, compiles, modules = [], [], []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if "[pipeline-wait]" in line:
                m = WAIT_RE.search(line)
                if m:
                    waits.append(m.groupdict())
            elif "[shader-compile]" in line:
                m = COMPILE_RE.search(line)
                if m:
                    compiles.append(m.groupdict())
            elif "[shader-module]" in line:
                m = MODULE_RE.search(line)
                if m:
                    modules.append(m.groupdict())
    return waits, compiles, modules


def stall_coverage(frames, intervals):
    """Seconds of stalls (frames >= STALL_MS) that overlap any of the (start, end) intervals."""
    covered = 0.0
    for at, ms in frames:
        if ms < STALL_MS:
            continue
        start, end = at - ms / 1000.0, at
        spans = sorted((max(s, start), min(e, end)) for s, e in intervals if s < end and e > start)
        last = start
        for s, e in spans:
            if e > last:
                covered += e - max(s, last)
                last = e
    return covered


def summarize(folder):
    frames_path, log_path = os.path.join(folder, "frames.csv"), os.path.join(folder, "kyty_log.txt")
    frames = read_frames(frames_path) if os.path.exists(frames_path) else []
    waits, compiles, modules = read_log(log_path) if os.path.exists(log_path) else ([], [], [])
    print(f"== {folder}")
    stall_s = 0.0
    if frames:
        ms = [m for _, m in frames]
        stalls = [m for m in ms if m >= STALL_MS]
        stall_s = sum(stalls) / 1000.0
        span = frames[-1][0] - frames[0][0] + frames[0][1] / 1000.0
        print(f"quadros {len(ms)} em {span:.0f} s, média {len(ms) / max(span, 1e-9):.1f} fps")
        print(f"frame time ms: p50 {percentile(ms, 50):.1f}  p95 {percentile(ms, 95):.1f}"
              f"  p99 {percentile(ms, 99):.1f}  máx {max(ms):.0f}")
        print(f"quadros > 50 ms: {sum(m > 50 for m in ms)}   > 100 ms: {sum(m > 100 for m in ms)}"
              f"   travamentos >= {STALL_MS / 1000:.0f} s: {len(stalls)} ({stall_s:.1f} s)")
    else:
        print("sem frames.csv (KYTY_FRAME_TIMES não estava ligado)")
    if waits:
        waited = [int(w["waited"]) for w in waits]
        print(f"[pipeline-wait] {len(waits)}: soma {sum(waited) / 1000:.1f} s, p50 {percentile(waited, 50)} ms,"
              f" p95 {percentile(waited, 95)} ms, máx {max(waited)} ms;"
              f" fila {sum(int(w['queued']) for w in waits) / 1000:.1f} s")
        by_reason = collections.defaultdict(list)
        for w in waits:
            by_reason[w["reason"]].append(int(w["waited"]))
        for reason, values in sorted(by_reason.items(), key=lambda kv: -sum(kv[1])):
            print(f"  {reason:<14} {len(values):>4} esperas  {sum(values) / 1000:6.1f} s")
    if compiles:
        total = sum(int(c["ms"]) for c in compiles)
        here = sum(int(c["ms"]) for c in compiles if c["where"] == "here")
        # O log começa em 50 ms nos exes até a fase 3 e em 10 ms a partir da fase 4.
        print(f"[shader-compile] {len(compiles)} na thread da GPU: soma {total / 1000:.1f} s,"
              f" máx {max(int(c['ms']) for c in compiles)} ms ({here / 1000:.1f} s traduzindo na própria thread)")
        on_workers = [c for c in compiles if c["worker_ms"]]
        if on_workers:
            print(f"  {len(on_workers)} compiladas num worker (KYTY_SHADER_WORKERS):"
                  f" {sum(int(c['worker_ms']) for c in on_workers) / 1000:.1f} s fora da thread da GPU")
        for c in sorted(compiles, key=lambda c: -int(c["ms"]))[:5]:
            print(f"  {c['stage']} 0x{c['hash']} {c['ms']} ms, {c['words']} palavras, {c['where']}")
    if modules:
        print(f"[shader-module] {len(modules)} módulos >= 10 ms na thread da GPU:"
              f" soma {sum(int(m['ms']) for m in modules) / 1000:.1f} s, máx {max(int(m['ms']) for m in modules)} ms")
    if frames and stall_s > 0:
        intervals = [(float(w["at"]) - int(w["waited"]) / 1000.0, float(w["at"])) for w in waits if w["at"]]
        intervals += [(float(c["at"]) - int(c["ms"]) / 1000.0, float(c["at"])) for c in compiles]
        if intervals:
            print(f"travamentos cobertos por espera/tradução: {stall_coverage(frames, intervals):.1f}"
                  f" de {stall_s:.1f} s")


def self_test():
    assert percentile([5, 1, 3, 2, 4], 50) == 3 and percentile([1, 2, 3, 4], 95) == 4
    line = ("[pipeline-wait] at=100.500 VS 0x4efbcf79c36d8adf PS 0x00000000000000ab words=94000"
            " reason=small-target unoptimized waited=1500 ms queued=10 ms compiling=1490 ms")
    m = WAIT_RE.search(line)
    assert m and m["reason"] == "small-target" and m["waited"] == "1500" and m["at"] == "100.500"
    old = line.replace(" at=100.500", "")
    assert WAIT_RE.search(old)["at"] is None
    c = COMPILE_RE.search("[shader-compile] at=7.250 Compute 0x00000000000000cd 80 ms words=1234 translated=here")
    assert c and c["where"] == "here" and c["ms"] == "80" and c["worker_ms"] is None
    c = COMPILE_RE.search("[shader-compile] at=7.250 ps 0x00000000000000cd 12 ms words=1234 translated=worker"
                          " compiled=worker(310 ms)")
    assert c and c["where"] == "worker" and c["ms"] == "12" and c["worker_ms"] == "310"
    m = MODULE_RE.search("[shader-module] at=9.000 ps 0x00000000000000cd 41 ms words=130000")
    assert m and m["ms"] == "41"
    # Um travamento de 2 s (98..100) com uma espera de 1,5 s dentro dele (98,5..100) e outra fora.
    frames = [(97.0, 16.0), (100.0, 2000.0), (100.016, 16.0)]
    assert abs(stall_coverage(frames, [(98.5, 100.0), (101.0, 102.0)]) - 1.5) < 1e-9
    assert abs(stall_coverage(frames, [(98.5, 100.0), (99.0, 99.5)]) - 1.5) < 1e-9  # sobreposição conta uma vez
    print("self-test ok")


if __name__ == "__main__":
    if sys.argv[1:] == ["--self-test"]:
        self_test()
    else:
        for folder in sys.argv[1:] or ["."]:
            summarize(folder)
