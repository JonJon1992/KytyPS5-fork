# Shader PR ports — 7 October 2026

Base: `9a9827f0`. These are bounded adaptations to this fork, not merges of the donor branches.
Existing edits to `LoadBdaInline` and the concurrent queue-submit counters were preserved.

## Sources and scope

- [Upstream #976](https://github.com/KytyPS5/KytyPS5/pull/976), head
  `140ca9fae79b10f884d3b39d53788b30f19d7826`: GPU-selected scalar DWORD buffer reads;
  formatted X/XY/XYZ/XYZW reads of identity-selected 32-bit components; `IMAGE_LOAD_PCK`.
  The fork's existing dynamic `ReadConstBuffer` path remains available.
- [Upstream #1118](https://github.com/KytyPS5/KytyPS5/pull/1118), head
  `54ee0da05c6f3b441592e12bed6ce038b604bc86`: selected changes to indirect image planning.
  Recognize nonzero proofs inside conjunctions, false disjunctions and FF1 sentinel checks;
  require every predecessor path at a join to carry the proof; allow scalar descriptor reads
  in dominating ancestors and induction increments before a synthetic latch.

The #1118 port is partial. Wider CPU pointer-table records, masked pointer-table selectors,
loop-selected material keys and sharing duplicate indirect descriptor plans were not imported.
The donor uses a newer descriptor-source schema and a different tracker. Buffer/table-write
restrictions and existing bindless compatibility checks remain in force.

## Memory and cache contracts

Dynamic formatted reads support complete 32-bit components with identity destination selectors.
Unsupported formats/selectors and null descriptors produce zero; a formatted read must have all
requested components in bounds. Raw wide reads retain per-component bounds. D16, typed loads,
sub-DWORD dynamic loads and dynamic stores are outside this port.

Packed image reads require a sampled, non-FMASK, 32-bit texel and DMASK X only. The renderer binds
an identity-swizzled `R32_UINT` view and the emitter returns the raw word. Normal reads still use
their format conversion. Packed identity participates in both texture memo keys and description
cache keys. The serialized IR and compiled metadata include the new flags; the program disk
cache format changes from 1 to 2 so older entries are rejected. The Vulkan view-format contract
was checked against [Khronos resources documentation](https://github.com/KhronosGroup/Vulkan-Docs/blob/main/chapters/resources.adoc)
via Context7.

## Validation

Focused failures were reproduced before implementation: scalar dynamic V# tracking was rejected,
MIMG opcode 0x02 was unsupported, and the additional guarded/ancestor/latch image cases were
rejected. The focused resource-tracking tests subsequently passed, including negative cases for
unrelated/zero guards, entry/exit bypasses, positive OR conditions and invalid loop increments.

The GPU regression group runs seven dynamic buffer cases and a packed image read under full
Vulkan validation. Each compilation also checks serialized plans, compiled metadata and
deterministic re-emission. A renderer integration case checks raw words through mapped guest
memory in both packed float and integer formats, using a non-identity guest swizzle and distinct
raw/converted memo keys.

The final emulator/test build completed successfully. All five focused CPU CTest entries
passed; the frontend optimization and runtime buffer-stride groups also passed. The final GPU
group passed, including a native float binding followed by a raw unsigned view of the same
guest memory, with zero Vulkan validation errors. The Windows-specific ABI assertions have
not been checked with a Windows build.

Commands registered with CTest:

```sh
ctest --test-dir _Build/linux-clang --output-on-failure -R \
  '^(resource_tracking_indirect_buffer|resource_tracking_indirect_image|shader_packed_image_decode|shader_indirect_buffer_loop|resource_materialization|shader_rdna2_compat)$'
```

GPU tests need access to the host Vulkan device; they cannot run inside the restricted sandbox.
The Vulkan loader warned about the unused DZN ICD. Focused GPU runs reported zero validation
errors. The broader `resource_tracking_tests` suite still reproduces its pre-port FMASK
`Value.cpp:188` assertion; `shader_cfg_tests --bindless-only` still reproduces the same pre-port
atomic-OR assertion. These are not claims that the entire suite passes.

Two test-fixture problems were identified during validation: image factories did not initialize
the output buffer descriptor, and the EXEC buffer-load test used zero-length descriptors while
asserting that live loads survived. New packed tests initialize their own output descriptor;
the EXEC test explicitly supplies nonempty input and output ranges.

## Yōtei corpus A/B

The same 97 captured shader binaries were run through full translation/resource planning with
the same SRT and bindless options, before the ports, after #976, and after the selected #1118
changes. This diagnostic does **not** execute the shaders or prove their final SPIR-V compiles
with the game's real descriptors and dispatch state.

| Build | Accepted by translation | Skipped |
| --- | ---: | ---: |
| Before | 84 | 13 |
| #976 | 84 | 13 |
| #976 + selected #1118 changes | 84 | 13 |

The skip set did not change. It includes `stage4_5f3fdf61a7ca4a20` and the three pixel shaders
`stage2_71e89fc643691400`, `stage2_9e65954b8155f8d0`, `stage2_ee46c23b23db9950`.
Individual reports identify `BUFFER_LOAD_USHORT` at PCs 0x1500, 0x760 and 0x656c in
`stage4_174082508acf6307`, `stage4_40395313615abcc8`, `stage4_6cc64dee32dc7094`; and
`BUFFER_STORE_DWORD` at PC 0x530 in `stage4_86da5eb7b8257bb0`. The first three require
sub-DWORD dynamic buffer reads; the last requires a dynamic store path. #976 does not provide
these operations. The direct-image loop in `5f3fdf61` also needs additional planning support;
removing its write/dependency restrictions would not be a valid fix.

No new game FPS, frametime, wait-rate or context-switch A/B measurement was performed. These
changes add recompiler support; an improvement in Yōtei performance or rendering is unproven.
