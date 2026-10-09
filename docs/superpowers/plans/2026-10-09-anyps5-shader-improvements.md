# AnyPS5 shader improvements

**Goal:** Adapt useful, testable improvements from AnyPS5 72cf6c3 to the existing Kyty pipeline.

**Design:** The approved comparison is the brief. Add an opt-in extended SPIR-V cleanup recipe to the current optimizer, preserving interfaces, specialization constants, precise arithmetic and fallback. Validate on CPU and GPU and measure compilation/module size. Bring the standalone AMD hardware oracle with upstream provenance. Inspect prepared specialization and VRAM shadow ownership before deciding whether either requires a separate architecture.

**Execution:** Inline in the existing feature checkout, preserving unrelated user edits and reusing its configured build. No install, commit or preset change until validation supports it.

- [x] Extended optimizer: failing fixture for redundant expressions and aggregate locals; option/fingerprint; bounded additional passes; both-mode interface/memory/precision tests and GPU comparisons. Disabled by default.
- [x] AMD hardware oracle: vendor the standalone tool and tests with provenance; test argument validation; probe hardware availability without installing dependencies. HSA execution remains unvalidated.
- [x] Compare prepared specialization and UnitShadow against existing ownership paths, record concrete integration constraints and any applicable bounded improvement.
- [x] Build emulator and focused test targets, run checks, record measured effects and limitations; review final diff.
- [x] Add seven missing instructions, validate GPU results and neighboring operations, then update the local installed executable with a backup.

## User steering

The user supplied the Adrenaline article claiming 100% shader instruction coverage,
then said "otimizar depois". Prioritize instruction compatibility now. Keep the
already implemented extended optimizer disabled by default, with no performance
preset changes. Add the five missing integer16 min/max/median operations and the
two 24-bit multiply-high operations, with decoder-to-GPU readback tests.

## Progress

- Extended optimizer RED: repeated multiply and branch-dependent array remained.
  GREEN: CPU contracts and 36 GPU executions (12 cases in three modes) passed.
- Optional hardware oracle imported with provenance; 13 offline tests passed,
  including assembly/link checks. No HSA runtime validation yet.
- Integer16 ternary RED: GPU test rejected opcode 0x352 at pc 0x2c before changes.
  GREEN: six operations and the existing captured median fixture passed on GPU,
  in both conservative and extended modes.
- Multiply-high24 RED: missing opcode 0x0a failed to consume its literal and the
  decoder rejected that literal as an instruction. GREEN: signed/unsigned cases
  passed with VOP2, VOP3, literals and source/destination aliasing.
- Final Release build passed; 14 focused CTest cases and three additional GPU
  regression selectors passed. Static review found no remaining actionable issue.
  Installed executable updated atomically; SHA-256 matches the build and the old
  binary is backed up. See docs/PORT-ANYPS5-2026-10-09.md for evidence and limits.
- Prepared specialization would require changing binding/resource metadata and
  generic SPIR-V emission; current Kyty already caches translated IR. UnitShadow
  overlaps existing GPU-only image/buffer transfers and versioned ownership.
  Neither architecture is replaced in this bounded compatibility integration.

## Constraints and evidence

The current conservative optimizer's four CPU CTest cases passed before edits. Existing edits are docs/LINUX-U59.md, src/common/platform/uffdWriteWatch.h, tools/u59-preset.json and untracked docs/CRASH4-PERFORMANCE-2026-10-08.md. They are outside this change.

Context7 returned SPIRV-Cross rather than SPIRV-Tools in two lookups. Use Khronos's official optimizer.hpp and the pinned local header for exact API contracts. No dependency updates. Extra passes stay optional until representative performance evidence exists; no FPS claim from word-count reduction.
