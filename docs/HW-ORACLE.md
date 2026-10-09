# Hardware oracle

`tools/hw-oracle/` runs a few lines of RDNA assembly on a local AMD GPU, one input row per lane, and returns what the
hardware computed. Use it to investigate instruction semantics, then pin the
measured rows as expected values in the relevant Kyty shader tests under `tests/`.
This is an optional developer tool with no emulator runtime dependency. Source
and license provenance are recorded in [tools/hw-oracle/README.md](../tools/hw-oracle/README.md).

Results describe the host GPU, and are evidence to compare against the guest ISA;
they do not establish PS5 equivalence. Instruction availability and behavior can
differ between GPU generations. Record the GPU model, target, toolchain, and all
mode settings alongside measured results. The runner selects the first HSA GPU
agent; it does not use Kyty's GPU selection option.

## Requirements

- Linux with the `amdgpu` driver and read/write access to `/dev/kfd` (usually the `render` group). Root is not needed.
- The ROCm HSA runtime with its headers: `libhsa-runtime-dev` (Debian, Ubuntu), `rocm-runtime-devel` (Fedora),
  `hsa-rocr` (Arch), or AMD's ROCm packages. A runtime under `ROCM_PATH` or `/opt/rocm` is used first. The rest of ROCm
  is not needed.
- A C compiler, and `clang` + `ld.lld` with the AMDGPU target (`llc --version` lists `amdgcn`). If they aren't in
  `PATH`, they are taken from `$ROCM_PATH/llvm/bin`.

The first run builds `oracle.c` into `$XDG_CACHE_HOME/kyty-hw-oracle/`, or
`~/.cache/kyty-hw-oracle/` when `XDG_CACHE_HOME` is unset (`HW_ORACLE_CACHE` overrides
the entire directory). The target is read from the GPU (`HW_ORACLE_TARGET`
overrides it, but does not select a different GPU). Python 3.9 or newer is required.

## Kernel

`template.s` wraps the body under test. Per lane:

- `v4`-`v7` hold the row's 4 input dwords. `v10`-`v25` start at 0 and are stored as the result (16 dwords).
- Leave `v0` (lane id), `v1` (input offset), `v2` (output offset) and `s[4:7]` (input and output addresses) unchanged.
- 4 KiB of LDS, or the group segment size `--lds` (Python `lds`) gives in bytes, up to 64 KiB. Rows run in workgroups of up to
  1024 lanes, padded to whole waves with zero rows.
- Bytes passed as `extra` follow the rows of each dispatch, at `s[4:5] + 16 * rows`: buffer contents, texels, etc.
  Build buffer and image descriptors in SGPRs from that address. A kernel that needs more than 4 input dwords per lane
  takes the rest from there too, for example `v_lshlrev_b32 v40, 3, v0` / `v_add_nc_u32 v40, 16 * rows, v40` /
  `global_load_dwordx2 v[8:9], v40, s[4:5]` for two more dwords per lane, with `rows` the padded count.

Every run must explicitly set all floating-point controls, including runs of integer instructions. The CLI flags
use hyphens; Python keywords use underscores. Record these settings alongside the GPU and measured results.

| Python keyword | Values |
| --- | --- |
| `ieee` | 0 or 1: IEEE mode disabled or enabled |
| `dx10_clamp` | 0 or 1: DX10 clamp disabled or enabled |
| `denorm32`, `denorm16` | 0: flush input/output; 1: preserve input, flush output; 2: flush input, preserve output; 3: preserve both |
| `round32`, `round16` | 0: nearest even; 1: toward +infinity; 2: toward -infinity; 3: toward zero |
| `fp16_overflow` | 0: overflow to infinity; 1: clamp computed overflow to the largest finite value (infinite inputs and division by zero still produce infinity) |

`denorm16` and `round16` control both f16 and f64. These values follow the
[LLVM AMDGPU kernel descriptor documentation](https://llvm.org/docs/AMDGPUUsage.html#amdhsa-kernel-descriptor).
Float atomics need `coarse`: on fine-grained system memory they do nothing.

## Use

From the repository root, run the deterministic integer smoke example:

```sh
python3 tools/hw-oracle/hw_oracle.py \
  tools/hw-oracle/examples/integer-smoke.s \
  tools/hw-oracle/examples/integer-smoke.rows --outs 2 \
  --ieee 0 --dx10-clamp 1 --denorm32 0 --denorm16 3 \
  --round32 0 --round16 0 --fp16-overflow 0 > /tmp/kyty-hw-oracle-smoke.out
diff -u tools/hw-oracle/examples/integer-smoke.expected /tmp/kyty-hw-oracle-smoke.out
```

The first output is bitwise XOR of the first two inputs; the second is addition
of the last two inputs modulo 2^32. The expected file is calculated from these
integer operations, not captured from a GPU. Add `--wave64` to check that mode.

For a floating-point probe:

```sh
cat > body.s <<'EOF'
  v_add_f16 v10, v4, v5
EOF
printf '0x3c00 0x3c00 0 0\n0x7e01 0x3c00 0 0\n' > rows.txt
python3 tools/hw-oracle/hw_oracle.py body.s rows.txt --outs 1 \
  --ieee 0 --dx10-clamp 1 --denorm32 0 --denorm16 3 \
  --round32 0 --round16 0 --fp16-overflow 0
```

prints one line per row with the output dwords in hex. From Python:

```python
import sys
sys.path.insert(0, "tools/hw-oracle")
from hw_oracle import run

rows = [(a, b, 0, 0) for a in values for b in values]
results = run(body, rows, wave64=True, ieee=0, dx10_clamp=1,
              denorm32=0, denorm16=3, round32=0, round16=0, fp16_overflow=0)
```

Usual workflow: run the instruction on rows that separate the candidate models (edge values, NaN payloads, rounding
ties), write a model that matches every row, implement it, and put measured rows in the execution test.

## Limits

Only compute kernels. Pixel-shader ops that read their inputs from LDS also work: `v_interp_*` reads the attribute
parameters from LDS at `M0`, so writing them there by hand is enough. Behaviour that depends on the graphics pipeline
(parameter cache, exports, rasterization, `SPI_*` state) can't be measured with it.

Argument handling and cache behavior can be tested without a GPU or HSA headers:

```sh
python3 tools/hw-oracle/test_hw_oracle.py
```

When `clang` and `ld.lld` with AMDGPU support are available, these tests also
assemble and link the condition-operand and integer smoke kernels for `gfx1036`
in wave32 and wave64. Assembly tests skip when the tools are absent; the other
tests still run. None of these tests dispatches a GPU kernel or compiles the HSA
C runner. Hardware validation requires running the smoke command above on a
machine meeting the requirements.
