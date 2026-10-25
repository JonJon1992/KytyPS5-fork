"""B1: writes <n>.<stage>.<variant>.spv for every recorded module in <dir> (wait_shaders.py output) by
running spirv-opt with the given passes, and prints how long each run took (process wall time, so
it includes ~10 ms of process start) and the words before and after.

Usage: python -I opt_variants.py <spirv-opt.exe> <dir> <variant> <spirv-opt pass>...
"""
import csv
import os
import re
import subprocess
import sys
import time


def main():
    opt, folder, variant, passes = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4:]
    out = csv.writer(sys.stdout)
    out.writerow(["file", "variant", "words_in", "words_out", "opt_ms", "error"])
    for name in sorted(os.listdir(folder), key=lambda n: (len(n), n)):
        if not re.fullmatch(r"\d+\.(vs|ps|cs)\.spv", name):
            continue
        src = os.path.join(folder, name)
        dst = os.path.join(folder, name[:-4] + f".{variant}.spv")
        start = time.perf_counter()
        run = subprocess.run([opt, *passes, "--target-env=vulkan1.3", src, "-o", dst], capture_output=True, text=True)
        ms = (time.perf_counter() - start) * 1000
        error = (run.stderr.strip().splitlines() or [""])[0] if run.returncode else ""
        words_out = os.path.getsize(dst) // 4 if run.returncode == 0 else 0
        out.writerow([name, variant, os.path.getsize(src) // 4, words_out, f"{ms:.0f}", error])
        sys.stdout.flush()


if __name__ == "__main__":
    main()
