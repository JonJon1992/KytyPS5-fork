# Demon compatibility and shader optimization

**Goal:** Implement useful improvements from KytyPS5-Demon f17993c1, including optimization as now requested.

**Design:** Preserve Kyty's save metadata and compute-wave policy. Correct save free capacity using actual payload usage. Adapt the donor's compact pixel Function LDS layout only when every LDS access proves an aligned scalar access at `(LaneId << 2) + offset`; retain original guest address masking and bounds. A default-enabled codegen switch supports A/B and cache separation.

**Execution:** Existing checkout and configured Release build; preserve unrelated user edits. Independent save implementation and shader safety investigation use separate agents; the root owns shader edits, CMake integration, the canonical build and GPU tests. No commit requested for this new work.

## Tasks

- [x] Save capacity: fail a regression for used payload blocks, preserve binary `sce_sys/blocks.bin`, integrate both reporting callers, test nested payload and error/overflow behavior.
- [x] Compact pixel LDS: first demonstrate existing 8192-dword allocation on a two-slot shader; add a conservative layout analysis and pointer remapping; retain 16-bit address wrap and 8192-dword bounds. Unknown address forms, subword/vector/atomic LDS and non-pixel stages fall back unchanged.
- [x] Add option/fingerprint tests and CPU eligibility tests; render original and compact variants and compare exact outputs. Measure allocated slots and emitted module sizes; distinguish this from measured FPS.
- [x] Build emulator and affected targets, run focused regressions, review changes, update the installed executable with rollback backup only after passing checks.

## Review focus

- Offsets that differ by 65536 alias after the existing address mask.
- Out-of-bounds accesses retain zero-load/no-store behavior; compaction must not replace guest bounds with compact array length.
- Every LDS instruction must qualify before any access is remapped.
- Save metadata is excluded from payload usage and existing metadata format remains readable.
- No performance claim from another GPU or a synthetic allocation count.

## Candidate decisions

- Wave32 lane-local lowering offers no benefit to the current AMD path, which already requests native wave64; do not change host wave policy.
- Static extraction is specific to Demon's Souls. Existing general precompile/prefetch/fast-first mechanisms remain in use.
- Rewriting a previously recorded staging copy changes what earlier draws observe. Do not copy the donor's streaming timing assumption into our versioned resource ownership without a dedicated synchronization proof.

## Evidence

RED/GREEN passed for save accounting, compact allocation, wrapped aliases and allocation ceiling.
Release build and 15 selected CTest cases passed. Seven pixel fixtures rendered in four configurations
produce identical words. The two-cell fixture uses 8 declared bytes versus 1280 with the previous
array shrink pass. Review's allocation-ceiling finding fixed with a failing/passing regression.

Additional DS/atomic regression reached BufferAtomicFMinExactRawGlcModes and failed equally with
compact mode on and off. Root trace: explicit user data replaced the harness defaults and left
the output descriptor's s51 zero; AppendStoreVgpr uses s[48:51], so stores were discarded.
Corrected both exact-word FMIN/FMAX fixtures to set the same s51 default as MakeNativeUserData.
Rebuild passed; all 57 additional GPU cases and all 15 selected CTest cases passed after
the fixture correction. Production atomic code is unchanged. Installed executable updated
atomically and SHA-256 verified, with prior executable backed up in the artifact directory.

Local artifacts: `_Build/demon-port-20261009/`.
