# Wolverine compatibility integration

Source: Senaxx/KytyPS5 `wolverine` at `6b3fbc83d3bf39c89b3bb1adc68b6e50ce66732f`.
Local baseline: `guest-sync-release-mem` at `fdf54d20`.

Integrate missing functionality into the existing renderer and immutable resource plans. Keep the current recorder, dependency tracking, caches, and per-thread evaluation scratch. Do not replace the renderer wholesale or claim game compatibility from CPU tests.

## Task 1: Shader function expansion

Port the upstream ShaderFunctions decoder and connect it to actual pipeline shader translation, including immutable and bounded table calls, miss traps, user-data/code dependencies, and invalidation when callees change. Preserve embedded fetch shaders and asynchronous translation. Add a focused executable regression test for direct/table calls, unresolved targets, and changed callee code. Update CMake and the codegen cache identity where needed. Preserve upstream attribution. Build the relevant tests and commit reviewed changes as requested.

## Task 2: Missing instruction support

Port the Wolverine BVH, flat memory and shader control-flow dependencies that are absent locally. Connect decoder, translator, IR, SPIR-V emission, serialization, and codegen cache identity. Reuse local memory safety and EXEC handling. Validate generated SPIR-V and include a focused regression check for each new instruction family.

## Task 3: Bindless resources

Port bindless image/sampler resource tracking and SPIR-V emission and the host descriptor table. Connect capability negotiation, pipeline layouts, descriptor materialization, dirty/mapping invalidation and recorder replay. Preserve per-dispatch resource snapshots and barriers. Update serialized program data. Validate CPU resource materialization and shader emission, and build the emulator.

## Task 4: SRT execution

Adapt native/replay SRT acceleration to immutable resource plans and per-thread scratch. Route reads through local memory readers and observers; preserve failed-read fallback and clean backing certification. Avoid unguarded guest memory reads and plan-global mutable evaluation state. Use differential and concurrent checks against the existing evaluator.

## Validation

Run focused tests followed by relevant existing suites and a full emulator build. Record pre-existing failures separately. GPU/game validation requires a supported device and Wolverine capture; report honestly if unavailable.
