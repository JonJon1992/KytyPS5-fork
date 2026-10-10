# Group B: bindless, texture cache, resource lifetime (Senaxx wolverine), triage 2026-10-10

Compared against:
- the main tree (`guest-sync-release-mem` working tree) for the texture cache, the buffer cache and the recompiler;
- the worktree `/home/jonathanbraga/kyty-bindless-wt` (`bindless-cow` plus the uncommitted v12–v16 changes) for bindless.

Paths below are relative to `src/graphics/` unless noted. "wt:" means the bindless worktree.
`git apply --check` fails for every PORT candidate, so each one needs a manual adaptation.

## How our bindless differs from Senaxx's

Our port (2aa43c39) of Wolverine's bindless already took a different design from Senaxx's:
- **Ours, eager per consumer:** every draw or dispatch that consumes a heap re-reads the heap and checks every key. The feedback buffer is only diagnostic (wt: `bindlessTable.cpp:96`).
- **Senaxx's, lazy and request-driven:**
  - The heap translation persists.
  - A shader that hits a pending key writes a feedback flag.
  - Once per frame, `ResolveBindlessRequests` settles at most 128 flagged keys from a host-cached feedback snapshot.

So every feedback-based commit (c1afa640, 8585096e's probe, 1ead0eb5's rolling scan, c5a6961b's E3/stale handling) has no direct counterpart in our tree.

## Per-commit table

| Commit | Subject | Verdict | Evidence / reason |
|---|---|---|---|
| c1afa640 | Feedback flags read from a host-cached snapshot | **HAVE** (placeholder) / **N/A** (snapshot) | Transparent-black placeholders with `KYTY_BINDLESS_DEBUG_COLORS`: wt `pipeline/bindlessTable.cpp:301-309`. The host never reads our feedback buffer, so the snapshot does not apply. |
| 4b0ffc1f | Check a heap's resolved images only when their state changed | **PORT-MED** (adapt, as part of c5a6961b's E5) | We have a global `Image::BindingStateGeneration` plus `KYTY_BINDLESS_COMMIT_DEDUP` (wt `descriptors.cpp:1536`, `:3112`). We still copy one `bindless_textures` entry per key into each prepared stage and walk it at commit. Senaxx passes heap pointers instead, and bumps the generation **only for bindless-pinned images**. Our generation bumps for every image: the 24–75k alias-owner bumps per 10 s are mostly non-bindless images. See the design section. |
| 1ead0eb5 | A key whose heap T# changed is settled again | **HAVE** (other mechanism) | Every consumer compares the current T# records with `heap.descriptors` (wt `descriptors.cpp:1692`) and re-resolves a changed key at once. No rolling scan is needed. |
| 2efb1cc1 | Bindless only for sampled-only handles; point-mip stand-in for explicit-LOD gathers | **HAVE** / **N/A** | `BindlessCompatibleUses` (`shader/recompiler/ir/passes/ResourceTracking.cpp:1099`) requires sampled reads only. We never reject an explicit-LOD gather with linear mip filtering: it only warns and reads the nearest mip (`ResourceMaterialization.cpp:1438-1450`), so the point-mip stand-in is not needed. |
| 29bd4508 | Never leave a slot on an image being destroyed | **HAVE** (other mechanism) | A key whose image is unregistered becomes a placeholder (wt `descriptors.cpp:~1995-2016`). `OnImageUnregistered` → `ReleaseImageSlots`/`ForgetSlot` (wt `bindlessTable.cpp:789`, `:654`). Slots are reused only after their tick completes (`AllocateSlot`, `:635`). Image destruction is deferred to the tick (`DeferOperation`). We rely on `PARTIALLY_BOUND` instead of repointing to the placeholder (BINDLESS doc). |
| 57f1c6c5 | Keep a released image or buffer until the GPU is done with its *last use* | **PORT-LOW** (adapt) | Ours erases at the end of the tick in which `DeleteImage`/`DeleteBuffer` was called (`textureCache.cpp:~1148-1160`, `bufferCache.cpp:1054`). There is no last-use check. Our asynchronous draw-prep can prepare against an image or buffer and commit in a later tick: the same use-after-free shape Senaxx matched with Aftermath dumps. It costs one tick store per Obtain/Transit and one `IsFree` at release, and is a cheap safety net against `DeviceLost`. Note: `Image::Transit` does not set `tick_accessed_last` in our tree, and buffers have no `last_use_tick`. |
| 1fef6648 | Release between guest command buffers only after the GPU is done | **HAVE** (effectively) | Our `CommandScheduler::Active()` stays true after the first `Begin` (`m_registers` is never cleared; `commandScheduler.h:128`). The deferred path is therefore always taken, and `UnmapMemory` runs `Finish` (`renderContext.cpp:310-315`). The `else erase` branch remains in `textureCache.cpp:~1160` and `bufferCache.cpp:1057`, but is only reachable before the first command buffer. Switching to an unconditional deferral (Senaxx's `DeferRelease`) would be a trivial hardening if wanted. |
| 50a04054 | `ResolveTexture` caches texture descriptions by T# and shader use | **HAVE** (stronger) | `TextureBindingMemo` (`host_gpu/renderer/pipeline/textureBindingMemo.h:69`) memoizes the whole resolution with hash and tag hints, and is two-choice in wt v7. |
| 9d25c3d7 | Overlapped aliases age by presented frames; skip unmaps of never-mapped ranges | **HAVE** | `AliasFramesBeforeRemoval = 4` and `KYTY_IMAGE_ALIAS_AGE` (`textureCache.cpp:79-85`, `:2043`), the alias-owner rule (`alias_owner`), and the unmap skip (`renderContext.cpp:299-307`). |
| a92cf441 | Drop an evicted image's write-back if guest bytes changed before it landed | **HAVE** (likely, other mechanism) / SKIP | Our eviction downloads go through `BufferCache::BeginBackingPublication` (`textureCache.cpp:5470`). CPU access to a range with a pending publication waits for it (`renderContext.cpp:257-261`). Low value on a 16 GB card. |
| 8585096e | Unsampled bindless textures freed under pressure; slots reused | **HAVE** (slot reuse) / **N/A** (probe) | Slot recycling after the tick (wt v6, `AllocateSlot`) is in place. The usage probe needs feedback-driven pinning; our bindless images are not pinned and age through the normal LRU. The memory concern still matters for us (see the design section). |
| 269b12f7 | The game's own samplers by default | **HAVE** | `KYTY_BINDLESS_SAMPLERS` defaults to 1 (`host_gpu/renderer/pipeline/pipelineCache.cpp:3869`; wt `:3882`). Sampler heaps are mirrored (wt `FindOrCreateSamplerHeap`/`MirrorSamplerHeap`). |
| c5a6961b | The engine method: heap entries translated when the guest writes them | **PORT-HIGH value / MED-HIGH risk** (adaptation of ideas, not code) | The best lead for Yōtei's Thread_Gpu. See the design section: port the per-image change list (E5) and heap-dirty tracking (E1/E2); do **not** port lazy feedback or E3. |
| 89b188d7 | Images the guest dropped from its heaps are freed (300-frame grace) | **SKIP** (for now) | It relies on Senaxx's `bindless_pinned` exemption from the collector. Our bindless images are never exempt, so the normal LRU and idle collectors (`RetireUnusedImages`, `textureCache.cpp:~6085`) already free them. Our heap eviction (v13) releases keys in bulk every second, so "no key refers" would fire constantly. Revisit only if we adopt pinning. |
| 3861fd94 | 6 GiB cap per cache; unsafe GPU-written images are kept | **PORT-LOW** (the "keep unsafe" guard only); **SKIP** (cap) | The stock collector (`textureCache.cpp:~6194-6205`) and the pressure collector (`:~6259-6270`) call `FreeImage` on a GPU-modified image that is *not* `SafeToDownload`, losing GPU-only contents. Senaxx saw missing glyph-atlas text. The fix is a `continue` when `!safe`. The 6 GiB cap targets 32 GB cards; on 16 GB we have the budget, idle and pressure policies already. |
| 48028863 | Evicted GPU-written images are freed only after their write-back lands; small targets stay | **PORT-LOW** (verify first) | Ours calls `DownloadImageMemory` and then `FreeImage` at once (`textureCache.cpp:6201-6205`, `:6266-6270`). Texture uploads do not wait for pending backing publications (no `PendingBackingPublication` or `SynchronizeGpuBackingForRead` in `textureCache.cpp`). A draw in the same tick could recreate the image from stale bytes. Only reachable under eviction pressure. Two-phase free or a wait on the pending publication would fix it. |
| a137a4ea | Patch logging takes no lock after 256 lines | **N/A** | We have no "Bindless patch" per-draw logging. |
| fa875b57 | Register clears of 16-bit-channel, 64-bit and B10G11R11 targets decoded | **HAVE** | `DecodePackedColorClear64` and `KYTY_CLEAR_REGISTER_WIDE` (`host_gpu/renderer/image/imageInfo.h:441-530`), used at `textureCache.cpp:223-228` with `dcc_clear_word1`. Senaxx also covers the CMASK key-0 path; ours is DCC 0x20 only, a minor gap. |
| 13abd5e2 | `KYTY_GPU_RESOURCES`: shaders read buffers through the descriptor (BDA) | **SKIP** | An opt-in per-shader experiment, "built, not yet run". BDA reads put more weight on our coherence and BDA-sync path, which is the Crash 4 bottleneck. Our IndirectBuffer path (`ResourceTracking.cpp:1650-1692`) already covers runtime V#s. |
| 5d09195e | `KYTY_GPU_RESOURCES` keeps descriptor-holding buffers bound | **SKIP** | Fix to 13abd5e2. |
| ff92ad41 | Block-compressed volumes: 2D views only where allowed (RADV) | **HAVE** | `host_gpu/renderer/image/image.cpp:104-130`. |
| 678c6e10 | Block-compressed volumes: drop optional flags until the device accepts | **HAVE** | Same place, the `drops[]` loop (`image.cpp:107-130`). |
| 3cd1568d | Buffers spill to system memory; buffer GC no longer exits; OOM messages | **PORT-LOW** (adapt, robustness) | We retry past the budget (`host_gpu/renderer/cache/streamBuffer.cpp:139-165`), but there is no explicit host-memory fallback. `RunGarbageCollector` still has `EXIT_IF(!DownloadBufferMemory(...))` (`bufferCache.cpp:5304`): the exit Senaxx removed. That path only runs under aggressive buffer GC, which is rare on 16 GB. `KYTY_VRAM_LIMIT_MB` is already here (`vma.cpp:270`). |
| 09bb5a9f | `FindImageFromRange` queries one page | **HAVE** | `textureCache.cpp:4145`. |
| 1f148af8 | Overlap check gathers written ranges once | **N/A** | This upstream per-draw scalar-read overlap check does not exist in our `descriptors.cpp`. Ours is `WrittenBuffersDisjoint` in `ResourceMaterialization.cpp`. |

## Senaxx bindless design vs bindless-cow

### Senaxx at c5a6961b

Per draw (`PrepareBindlessHeaps`), the cost is O(heaps used), not O(keys):
- `FindOrCreateHeap` (a linear scan);
- `NeedsWatch`/`WatchHeap` the first time;
- patch (region, count).

No guest heap bytes are read, and no key is resolved, touched or transitioned per draw.

Keys are settled in four ways:

1. **Lazy (default).** A shader that samples a key whose translation is `Pending` writes 1 to `feedback[region+key]`.
   - Once per frame, `ResolveBindlessRequests` reads a host-cached snapshot of the feedback buffer (c1afa640).
   - It resolves at most 128 flagged keys (`ResolveBindlessKey`: read 32 B, `ResolveTexture`, `FindTexture`, `AllocateSlot`, `WriteSlot`, pin the image, `SetTranslation`).
   - A new key samples the placeholder for one or two frames.
2. **E1/E2 (the engine method).**
   - From the first draw that uses a heap, its guest pages are write-watched through `PageManager::UpdatePageWatchers<true>`.
   - A CPU write fault (`RenderContext::HandleFault`/`InvalidateMemory` → `BindlessTable::OnCpuWrite`) unwatches the page and marks it written.
   - At the start of every guest submission (`GuestGpu::Process` first slice → `SyncBindlessHeaps`), the written pages are re-watched and only their entries are compared with the mirrored T#.
   - A changed key is released and settled again. Changes are classified as view-only (min LOD, levels, swizzle: streaming) or resource changes.
   - This replaces the rolling 2,048-keys-per-frame scan, which took up to 32 frames.
3. **E5.** `NoteBindlessStateChange` fires only for `bindless_pinned` images and pushes `Image::self_id` into a global list.
   - `CommitBindings` transitions only `heap->unchecked` (newly settled images) and that change list, instead of every resolved image of every heap whenever anything changed.
   - The old pass walked ~1,400 images per draw.
4. **E3 (eager, off by default).** Under a per-submission budget, it settles keys no draw asked for.
   - It is off because it held about 3 GB more VRAM (9.3 GB against 6.0 GB of heap textures) for the same fps.

Lifetime:
- Pinned images are exempt from the collector.
- 8585096e's usage probe and 89b188d7's guest-drop grace free them.
- 29bd4508 repoints a slot to the placeholder before its image is destroyed.

Limits:
- 32-byte T# stride only (`key*32`).
- CPU writes only: no hook for GPU-written heap pages.
- A linear heap search.

### bindless-cow (wt, v16)

Per consumer, the cost is O(keys):
- read the T# records (from certified clean backing, syncing GPU-dirty pages first);
- compare them with `heap.descriptors`;
- then repeat the heap whole (`TryRepeatKeys`), or each key (`TryRepeatEachKey`, plus the `TICK_REPEAT` stamp at the global `BindingStateGeneration`), or resolve fully;
- push one `bindless_textures` entry per key, which the stage commit walks (`TouchImage`, `Transit`, deduplicated);
- publish the translation with copy-on-write regions.

Strengths Senaxx lacks:
- any record stride (Yōtei: 440- and 872-byte records);
- GPU-written heaps are handled;
- `HeapPlace` index and idle-heap eviction;
- copy-on-write, so in-flight draws never see a changed translation;
- no placeholder frame for new heaps.

That last point matters for Yōtei. It creates descriptor tables at new addresses all the time: ~500 new heaps/s, 1,200–1,700 live. A lazy design would show the placeholder on every new heap's first one or two frames.

Cost, from wt v12/v14 profiles of Thread_Gpu:

| Item | Share of Thread_Gpu |
|---|---|
| `PrepareBindlessHeaps` | ≈25% |
| └ `TryRepeatEachKey` | 8–11% |
| └ `TouchImage` | 7.5% |
| `CommitBindings` | ≈16% |
| └ `TouchImage` | 5% |
| └ `Transit`/`GetBarriers` | 4% |

`TICK_REPEAT` fails because the global generation moves 15–18k times per 10 s. The bumps come mostly from images that are not in any heap: alias owner, structure, partner, buffer-modified.

### What maps onto bindless-cow (recommended order)

1. **Per-image bindless invalidation (E5 + 4b0ffc1f). Low risk, highest value per line.**
   - Give `Image` a `bindless_refs` flag or count, set by `AddImageReference`/`AddSlotOwner` and cleared when the last reference goes. Keep `self_id`.
   - Route `NoteBindingStateChange` through an instance method. Bump a *bindless* generation, or push `self_id` to a change list, only when the image has bindless references. All other images stop invalidating heaps.
   - Per consumer, the keys to re-check are `m_image_refs[changed ids]` (already kept in `BindlessTable`), not all keys. The `TryRepeatEachKey` sweep becomes "nothing changed, repeat".
   - Expected effect: most of the 8–11% `TryRepeatEachKey` and the `TICK_REPEAT` resets go away.
2. **Heap-dirty tracking instead of re-reading records per consumer (E1/E2 adapted).**
   - Mark a heap dirty when:
     - (a) a CPU write fault or invalidation hits its pages: hook `RenderContext::HandleFault`/`InvalidateMemory`, as Senaxx does with `OnCpuWrite`. Heap pages usually sit in tracked buffers that already fault, so the extra cost is a range test;
     - (b) the GPU writes its pages: hook `BufferCache::MarkGpuWritten` or the coherence GPU-dirty bits. Senaxx has no equivalent, and Yōtei needs it;
     - (c) an unmap or eviction.
   - A clean heap with no changed referenced image skips the record read, the compare and the per-key checks entirely, and re-uses its last published region.
   - Keep our eager first resolution of new heaps and our copy-on-write publish. A clean heap needs no new publish, so copy-on-write moves drop too.
   - To check first: whether the existing memory-tracker page versions (`NoteStructureChange` page versions) can serve as a cheaper "written since" signal than a new watch.
3. **Per-heap commit list.**
   - Store the deduplicated (image, layout, range) set per heap. Rebuild it only when the heap or its images changed. The stage commit then iterates heaps instead of ~1,500 per-key entries.
   - Touch the LRU once per heap per frame instead of per key per consumer: this removes the `TouchImage` 7.5% + 5%.
4. **Do not port** lazy feedback settling or E3 as the default. New heaps would sample the placeholder for frames, which is a correctness regression for Yōtei's transient tables.
   - Consider Senaxx's VRAM point separately. Eager settling of every heap key costs memory (+3 GB in Senaxx's measurement), so measure Yōtei's VRAM with `KYTY_VRAM_STATS`. A later hybrid could settle eagerly only keys a draw has sampled (feedback as a usage signal), for eviction only.

### Expected payoff

1–3 together target ~30–35% of Thread_Gpu (`PrepareBindlessHeaps` 25% plus the bindless part of `CommitBindings`).

Thread_Gpu also stalls ~25–30% on syncs: `ReadMemoryDrain`, `HandleFault` from the SRT walker, and the `MaterializeDccClear` CPU fallback. These are unaffected. So a realistic estimate is +25–50% fps (about 9.5 → 12–14), not a return to the 13–21 fps of bindless off.

`MaterializeResources` (16% in dispatches) is a separate front.
