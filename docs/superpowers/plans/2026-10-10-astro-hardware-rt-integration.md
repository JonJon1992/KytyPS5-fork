# Astro hardware RT integration

Goal: replace Astro's guest BVH traversal with native ray queries while preserving
the game's leaf IDs, instance transforms, facing filters and live shader outputs.

Architecture: explicit Astro leaf-list format in immutable BVH snapshots; native
non-opaque candidate processing; coherent GPU capture of geometry, lists and
instance context; only a validated shader recognizer may replace traversal.

Tech stack: C++20, Vulkan KHR acceleration structures / ray query, GLSL, SPIR-V.

Spec: docs/HARDWARE-RT-BACKEND-2026-10-10.md and the saved disassembly of shader
7d6ab87e6c984bb7. These documents describe experimental work, not gameplay proof.

Global constraints: do not use CPU-after backing as a live snapshot; do not
activate replacement with incomplete scene data; preserve the software path.
Keep descriptor ownership and scheduler completion leases in the renderer.

Review focus: list termination, bounds, repeated leaf IDs, missing pages, nearer
rejected hits, transform inversion, mirrored facing, equal-distance hit identity.

- [x] Task 1: add failing leaf-list conversion tests, then decode Astro's BLAS
  header and bounded signed-ID lists. Preserve node and list-ID remapping.
  Interface: BvhSnapshot format, Primitive leaf_id, PreparationCache.
  Verify: guest_bvh_conversion_tests, including truncated list and cache changes.
- [x] Task 2: add failing native policy tests, then process non-opaque triangle
  candidates before confirming hits. Preserve two-sided / facing selection.
  Interface: per-query policy, scene metadata, native compute shader.
  Verify: hardware_rt_backend_gpu/off under Vulkan validation.
- [x] Task 3: capture complete resident BLAS and 160-byte instance context from
  the executing GPU dispatch, without changing the software result. Validate
  complete snapshots and compare full subtree results with software traversal.
  Interface: versioned bounded capture sidecar, exact shader/PC context.
  Verify: capture fixture with missing/cross-page memory; real Astro capture.
- [x] Task 4: recognize the game's traversal and connect native results to its
  live outputs, with coherent scene preparation and explicit fallback on any
  unsupported rule. Verify same-scene software/hardware image and stability.
  Accepted for the bounded prototype: shader 7d6ab87e6c984bb7, wave32/wave64,
  one captured single-list BLAS, current GPU bytes checked on every use. Real
  gameplay counters increased and the user confirmed normal image after walking.

Remaining for general hardware RT: coherent dynamic import of other/larger
BLAS, support for ray-dependent multi-box/list order and other shader rules,
native replacement of the TLAS, and a controlled FPS comparison without
diagnostics. The present acceptance does not complete these broader items.

Progress and measured limitations are recorded in the backend research document.

Stopped at the user's request after code commit 90200ebf and gameplay validation.
The game exited normally (0); final counters were 24,638,470 native results and
472,193,263 software fallbacks. Resume with dynamic coherent BVH import and
validation-cost reduction; general hardware RT and FPS gains remain incomplete.
