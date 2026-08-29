# BFS live root auto-grow — design (items 2 + 3)

Target: a canonical AMI booted on a LARGER root EBS grows its BFS root to fill
the disk on first boot (19→40 GiB), no rebake. Item 1 (in-bitmap-capacity grow,
~≤32 MiB, no relocation) is the merged foundation; this document designs the
**large** grow (item 2) and the block-cache resize it needs online (item 3).

> POSTURE: this rewrites live root-filesystem metadata. A bad grow bricks every
> instance from the AMI. DESIGN-FIRST, incremental, **fault-injection-proven**,
> PROPOSE-ONLY. The on-disk mutation stays gated (`B_NOT_SUPPORTED`) until the
> crash-injection acceptance below passes.

## On-disk layout (confirmed from bfs.h / Volume.cpp / BlockAllocator.cpp)
```
block 0            superblock (+ boot block)
block 1 ..         block bitmap  = ceil(num_blocks / (block_size*8)) blocks,
                                   grouped into num_ags AGs of blocks_per_ag each
bitmapBlocks+1 ..  log area (log_blocks: block_run; log_start/log_end), fixed len
after log ..       data area (dynamically allocated)
```
- `num_ags = divide_roundup(num_blocks, blocks_per_ag * block_size * 8)`, enforced
  by `disk_super_block::IsValid()` (Volume.cpp:55) — a grow MUST keep this invariant.
- `blocks_per_ag` (bitmap blocks per AG) is fixed at format time; `num_ags` scales
  with `num_blocks`. So a large grow ADDS bitmap blocks and AGs.
- The bitmap is at a FIXED offset (block 1). Extra bitmap blocks land at
  `[oldBitmapBlocks+1, newBitmapBlocks+1)` — a region that today holds the **log**
  and possibly **data**. That collision is the whole problem.

## Item 2 — the grow algorithm
Inputs: `newNumBlocks` (target, = partition size ≫ current). Compute
`newBitmapBlocks`, `newNumAGs`, and the new log location.

1. **Guard / classify.** If `newBitmapBlocks == oldBitmapBlocks` → item 1 path
   (already merged). Else this large path. Refuse shrink.
2. **Reserve new homes in currently-FREE space (non-destructive):**
   a. a new **log** region of the same length, and
   b. relocation targets for any **data** blocks currently sitting in
      `[oldBitmapBlocks+1, newBitmapBlocks+1)` (the region the extended bitmap
      will claim), and for the OLD log region if it's inside that range.
   Nothing written here touches the old FS — pure allocation in free space.
3. **Evacuate data** out of the to-be-bitmap region: copy each occupied block to
   its reserved target and rewrite the owning inode's `block_run`(s). This is the
   journal-sensitive step — it MUST go through BFS's transaction/log so a crash
   leaves either the old or the new block_run, never a dangling one. (In practice
   a freshly-baked root has little/no data in this low region — the region is
   mostly the old log + free — which bounds the evacuation, but the algorithm
   must handle the general case.)
4. **Relocate the log:** copy/replay the log to the reserved new region; point
   `log_blocks`/`log_start`/`log_end` at it (in the staged superblock, not yet
   committed).
5. **Write the extended bitmap** into `[oldBitmapBlocks+1, newBitmapBlocks+1)`
   (now evacuated): initialize the new bitmap blocks (reuse
   `BlockAllocator::InitializeAndClearBitmap` group-init logic), marking used =
   {new bitmap blocks, new log, relocated data}, free = the rest up to
   `newNumBlocks`.
6. **Atomic commit:** ONE `WriteSuperBlock()` updating `num_blocks`, `num_ags`,
   `log_blocks/log_start/log_end`. This single block write is the commit point:
   before it the old geometry is authoritative; after it the new one is.
7. `BlockAllocator::Reinitialize()` to pick up the new free space (in-memory).

## Crash / rollback safety (the acceptance-defining hard part)
The danger window is step 5→6: writing the new bitmap over the old log/data
region is destructive, and the old superblock still points into it. A power loss
there must not corrupt.

Design: a **grow-in-progress journal** independent of the (being-relocated) main
log. Options, in preference order:
- **Offline grow (item 4 path, PREFERRED for safety):** run the grow BEFORE the
  root is mounted read-write (early-boot phase / bfs_shell), single-threaded, no
  concurrent writers — eliminates item 3 entirely and shrinks the danger window
  to the metadata writes, which a small intent-log makes idempotent-replayable.
- **Online grow:** needs (a) item 3 (cacheable tail) and (b) serialization
  against in-flight transactions. Higher risk; defer behind offline.
Either way: write a small persistent **intent record** (target geometry + phase)
to a reserved block BEFORE step 5; on mount, a recovery routine detects an
incomplete grow and either completes it (idempotent replay) or rolls back to the
old superblock (which is untouched until step 6). Never half-written.

## Item 3 — block_cache resize (online path only)
`block_cache_create(fd, numBlocks, ...)` fixes the max block at mount; there is
no resize (confirmed in headers/os/drivers/fs_cache.h). Blocks beyond mount-time
`numBlocks` are not safely cacheable → an online grow can't write the new tail.
Add:
```
status_t block_cache_set_size(void* cache, off_t newNumBlocks);
```
in fs_cache.h + the block_cache implementation: grow the cache's internal
block-count bound, serialized against in-flight transactions (block the resize
until the cache is transaction-quiescent, or fail with B_BUSY). BFS calls it
after step 6 on the online path. The OFFLINE path does not need this (root not
mounted r/w), which is the main argument for doing offline first.

## Scope guard
This design is item 2 (+3). It is NOT auto-mergeable and NOT auto-implemented in
full: the destructive path (steps 3–6) stays `B_NOT_SUPPORTED` until the
crash-injection acceptance in the verification plan passes on real Graviton.
