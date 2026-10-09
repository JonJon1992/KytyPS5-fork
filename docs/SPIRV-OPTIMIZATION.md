# SPIR-V optimization

Guest shaders receive a conservative SPIRV-Tools cleanup immediately after emission in
`ShaderRecompiler::CompileProgram`, before validation, Vulkan shader-module creation and storage
in the persistent program cache. Both the instrumented module and its optional plain
mip-statistics variant use the same cleanup.

The pass sequence forwards local loads/stores, removes dead branches/instructions, cleans up
unreachable blocks, merges blocks and deduplicates constants/types. It preserves descriptor
bindings, entry-point interfaces, specialization constants, memory side effects and precision
decorations. It does not use the broad `-O` recipe, unroll loops or reassociate floating-point
arithmetic. Input and output are validated for Vulkan 1.3; if optimization fails, the original
words remain intact and a warning identifies the shader and variant.

Optimization is enabled by default. To compare with the original emission:

```bash
KYTY_SPIRV_OPT=0 ./kyty_emulator [arguments]
```

`KYTY_SPIRV_OPT=1` enables it explicitly. Options are read once at startup. The codegen version
and this switch participate in the persistent cache identity; optimized words are saved and
reloaded without another optimizer run. Changing the switch invalidates incompatible cached
programs automatically.

`KYTY_SPIRV_OPT_EXTENDED=1` enables an experimental additional recipe: scalar replacement of
function-local aggregates with a 64-member limit, access-chain conversion, local SSA and
redundancy elimination. It is **off by default** and has its own cache fingerprint field.
It preserves the same contracts and fallback, without inlining, loop unrolling, CCP or
algebraic simplification. `KYTY_SPIRV_OPT=0` bypasses both recipes. See the
[AnyPS5 integration measurements](PORT-ANYPS5-2026-10-09.md): smaller modules came with
additional optimizer CPU time, and no game FPS improvement has been established.

The shader log reports the original/final word counts and optimization time in microseconds
(`SPIR-V optimize`). A smaller module is not evidence of higher FPS; compare cold compilation,
warm-cache behavior, GPU time and rendered output on the same workload and device.

Focused verification uses `spirv_optimizer_tests` (cleanup, binding/specialization preservation,
precision, memory effects, failure fallback and the environment switch), the `program_cache`
test, and `shader_recompiler_compute_tests --spirv-optimization-only` (original/optimized GPU
readbacks for twelve integer, float, atomic, loop and LDS cases). `shader_cfg` inspects the raw
emitter's exact CFG/instruction shapes; `lod_stats_codegen` validates optimized instrumented
and plain mip-statistics variants.

## Compact pixel LDS

`KYTY_FUNCTION_LDS_COMPACT` defaults to `1`. Pixel shaders whose complete LDS
access set proves scalar lane-relative addressing use one Function-storage cell
per distinct wrapped offset. Original guest bounds and address masking remain
unchanged; incompatible layouts retain their original storage. Set `0` to disable.
The option participates in the program-cache fingerprint. See
[the Demon integration report](PORT-DEMON-2026-10-09.md) for GPU equivalence tests,
allocation measurements and limits.
