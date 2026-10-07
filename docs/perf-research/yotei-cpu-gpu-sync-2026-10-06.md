# Ghost of Yōtei: CPU↔GPU synchronization, 2026-10-06

This report covers PPSA05512, eboot SHA-256
`803466729b58db40882062edf38417855658f51c9a8c7c39992b5a97153bd299`
on the RX 9070 XT. The first screenshot's ~6 FPS and ~2,099 waits/s came from a
different run and scene state. The comparisons below use one binary with live
A/B/B/A switches or report separate launches without claiming a controlled gain.

## Measured wait origins

`KYTY_SYNC_WAITS=1` records CPU call site, reason, wall duration, frequency,
thread, timeline tick and resource address when available. The opt-in trace is
`trace/syncwaits.csv`; `trace/syncwaitstats.csv` reports dropped rows. The
steady-state origin sample is kept locally under
`_Build/cpu-gpu-sync/origin-callsite-profile/` (35–60 s, 23.24 FPS, 40.5k
context switches/s, 3,575 write-tracking faults/s). Representative native
timeline waits:

| Cause and CPU call site | Thread | Waits/s | Blocked ms/s | Tick/resource |
| --- | --- | ---: | ---: | --- |
| Eager readback publication, `bufferCache.cpp:2547` | GPU completion | 541 | 371 | Timeline tick; guest page examples in trace |
| Direct readback drain, `bufferCache.cpp:1907` | Thread_Gpu | 356 | 258 | Current submit tick; GPU-written range |
| Eager readback consumed by a guest fault, `bufferCache.cpp:2249` | Thread_Main | 34 | 159 | Eager tick and page |
| Ordinary publication callback, `bufferCache.cpp:1122` | GPU completion | 317 | 46 | Callback tick and range |

The DCC metadata CPU readback at `textureCache.cpp:3277` occurred 23/s and
occupied 188 ms/s in a separate enclosing readback scope. Scopes can nest or
run concurrently, so their wall times must not be added together. Native
semaphore waits and host publication waits have separate monotonic counters;
the overlay's CPU-wait duration includes both. `vkQueueWaitIdle` and
`vkDeviceWaitIdle` were not identified in the steady-state draw path; queue
idle calls are associated with swapchain/shutdown handling.

## Experiments and decision

The publication dependency experiment replaces the GPU thread's duplicate
native wait for a downloaded range with the priority callback's completion
dependency. `FlushAndWaitPriorityPublication` still submits the current tick,
waits for all priority callbacks, and falls back to a native wait if none owns
that tick. The scheduler test verifies two callbacks, normal deferred cleanup,
the stable command wrapper and the empty-queue fallback. This path is **off by
default** (`KYTY_READBACK_WAIT_PUBLICATION=0`): the same-process A/B/B/A
profile did not improve frame time.

| Publication wait (25 s windows) | A1 off | B1 on | B2 on | A2 off |
| --- | ---: | ---: | ---: | ---: |
| FPS | 22.28 | 20.48 | 21.96 | 22.44 |
| Native GPU waits/s | 1,266 | 861 | 907 | 1,270 |
| Total CPU GPU-dependent wait ms/s | 850 | 754 | 845 | 852 |
| Context switches/s | 40,356 | 37,777 | 39,802 | 40,569 |
| Write-tracking faults/s | 3,473 | 3,178 | 3,458 | 3,522 |

The visible native count fell by about 30%, but some waiting moved to the
publication condition variable. FPS was lower in both B windows and the
total blocked time was not consistently lower. This does not satisfy the
50% wait reduction or <250 ms/s target and is not enabled for users.

An opt-in GPU inspection of VideoOut DCC keys (`KYTY_DCC_VIDEOOUT_GPU=1`)
eliminated that metadata readback while preserving its guest clear key in a
targeted Vulkan test. The warm A2/B2 windows were 16.44/16.28 FPS,
951/906 native waits/s, 925/939 total blocked ms/s and 34.4k/35.0k context
switches/s. It also remains **off by default**: removing one readback did not
produce a reliable frame-time gain.

## Graphics mode and limitations

The exact-version patch
[`tools/patches/PPSA05512-performance-no-rt.json`](../../tools/patches/PPSA05512-performance-no-rt.json)
changes the game's saved-mode input to Performance index 2 before it derives
mode flags. Runtime reads showed desired/current/previous mode all 2 through
40 s; a forced RT control showed all 3 before a Vulkan device loss at 15 s.
The mode-2 path excludes the game's RTGI mode range (3–5), but visual
correctness and the absence of every RT/BVH dispatch have not been established.
The patch does not force 2560×1440 internal resolution. An earlier direct
1440p table edit crashed in guest code and was discarded.

The final Performance run was 16.15 FPS, 760 native waits/s, 450 ms/s of
total GPU-dependent CPU waiting, 37.0k context switches/s and 2,061
write-tracking faults/s over 10–30 s. It meets neither initial wait target.
An unpatched run after the first Performance test also selected mode 2, which
shows that graphics settings can persist between launches; it is not a valid
RT-versus-Performance A/B. No causal FPS or crash improvement is claimed.
