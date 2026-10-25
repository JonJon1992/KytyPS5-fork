# Shader Gameplay Stutter Implementation Plan

> **For agentic workers:** execute inline in this worktree; preserve the existing working tree.

**Goal:** Reduce stalls from graphics shader-cache reads and prioritize shader work requested by the current scene, with an opt-in fast graphics pipeline that is replaced by its optimized version.

**Architecture:** Put asynchronous disk reads beside `ProgramCache` and its existing translation workers. Add request-age scheduling to the existing translation and pipeline queues, preserving urgent graphics jobs and reserving a small worker subset for stale work. Keep the temporary unoptimized graphics pipeline path disabled unless `KYTY_PIPELINE_QUICK=1`, and retain replaced handles until pipeline-cache teardown.

**Tech Stack:** C++20, `std::thread`, atomics, condition variables, Vulkan pipeline creation.

**Spec:** `docs/SHADER-STUTTER-PLAN.md` and the approved approach in this conversation.

## Global Constraints

- Shader disk reads are asynchronous only for deferrable, non-compute stages; verification mode stays synchronous.
- `KYTY_SHADER_READ_THREADS=<n>` enables disk-read workers (capped at 16). Default 0 since the Claude review of 2026-10-24: a read in flight skips the draw at every run's first use of a cached shader, memory-writing draws included, so it stays opt-in until a session validates it.
- `KYTY_PIPELINE_QUICK=1` enables temporary unoptimized graphics pipelines; default remains off.
- Keep compute dispatch behavior unchanged; never skip a dispatch to hide compilation.
- Do not add or run tests in this task; run the emulator build and `git diff --check` only.

## Review Focus

- A pending disk read must outlive neither its `Store` nor its worker; consume results only after acquire/release publication.
- Repeated draws must raise queue priority without starving jobs that leave the visible scene; worker 0 keeps oldest-first service.
- A quick pipeline can be submitted before replacement; retain its Vulkan handle until shutdown to avoid destroying an in-flight pipeline.
- Disk verification and shader-debug-dump modes must retain their existing behavior.
- Stopping workers must leave pending handles owned and destroyed exactly once.

---

### Task 1: Asynchronous shader disk-cache reads

**Files:**
- Modify `src/graphics/host_gpu/renderer/pipeline/pipelineCache.cpp`

- [x] Add a bounded `DiskReaders` queue, per-key pending-read state, and `KYTY_SHADER_READ_THREADS` configuration.
- [x] Route only deferrable graphics source/permutation loads through workers; annotate and skip the draw while a read is pending.
- [x] Preserve hit/miss counters, verification mode, shutdown order, and the synchronous fallback when the pending-read map is at capacity.
- [x] Review worker ownership and `Store` lifetime; compile the emulator target and run `git diff --check`.

### Task 2: Request-recency scheduling

**Files:**
- Modify `src/graphics/host_gpu/renderer/pipeline/pipelineCache.cpp`

- [x] Stamp translation and pipeline jobs when submitted and refresh the stamp when a draw asks again.
- [x] Let worker 0 take the oldest queued job and other workers take the most recently requested job.
- [x] Limit stale pipeline work and optimized warm-ups to the first two workers by default; preserve urgent-job precedence and FIFO warm-up behavior.
- [x] Compile the emulator target and run `git diff --check`.

### Task 3: Opt-in quick graphics pipeline with replacement

**Files:**
- Modify `src/graphics/host_gpu/renderer/pipeline/pipelineCache.cpp`
- Modify `src/graphics/host_gpu/renderer/pipeline/pipelineCache.h`

- [x] When `KYTY_PIPELINE_QUICK=1`, compile an unoptimized pipeline first, publish it to draws, then queue the optimized build behind requested work.
- [x] Swap to the optimized handle on the GPU thread when ready and retain the old handle through cache teardown.
- [x] Keep the existing urgent-fast path unchanged when the option is off; avoid duplicate cache warm-ups when quick replacement is active.
- [x] Compile the emulator target and run `git diff --check`.

### Verification record

- `ninja -C _Build/port -f build-agent.ninja -j8 kyty_emulator` completed successfully after adapting only local copies of the generated Ninja manifest and included rules to the available CMake/clang tools. The original generated files were left unchanged.
- `git diff --check` completed successfully. No tests were added or run, per the plan constraints.
