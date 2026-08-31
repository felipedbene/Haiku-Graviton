# BFS live root auto-grow — design v2 (items 2 + 3, post-review)

Target: a canonical AMI booted on a LARGER root EBS grows its BFS root to fill
the disk on first boot (19→40 GiB), no rebake. Item 1 (in-bitmap-capacity grow,
~≤32 MiB, no relocation) is the merged foundation.

**v2 changes the strategy:** the shipping path no longer relocates anything at
grow time. Relocation is moved to *format time* (bake the image with the log
placed high and a reserved bitmap-growth gap), and the grow executes at
*mount time* before the block cache and journal go live. The general
relocating grow (v1 steps 3–4) is retained as a design appendix but stays
permanently gated. Item 3 (`block_cache_set_size`) is **dropped** — the mount-time
execution point makes it unnecessary.

> POSTURE unchanged: this rewrites live root-filesystem metadata. A bad grow
> bricks every instance from the AMI. DESIGN-FIRST, incremental,
> **fault-injection-proven**, PROPOSE-ONLY. Any destructive path stays gated
> (`B_NOT_SUPPORTED`) until the crash-injection acceptance in the verification
> plan (v2 deltas below) passes.

## Shipping status (2026-08-31)

The first-boot **partition** grow that T4 depends on has moved out of userland and
into the EFI boot loader: `EfiDevice::GrowBootGptPartition()`
(`src/system/boot/platform/efi/devices.cpp`) extends the trailing BFS GPT partition
to fill the disk *before* the kernel mounts the boot volume, so the existing
mount-time BFS grow fires on **boot one** — no reboot, no `partition_grow` /
launch-daemon ordering dependency (that userland job stays as a harmless idempotent
fallback). arm64-gated, grow-only, trailing-BFS-only, idempotent, and a no-op on any
GPT/geometry/CRC inconsistency so it never bricks boot.

- **Build-verified** against the arm64 cross-tools: `haiku_loader.efi` links clean.
- **In-place append grow** (19→40 GiB, mount-time engine) was previously
  hardware-proven (checkfs clean across crash-states + power-cut).
- **Owed:** a hardware one-boot confirmation of the *loader-driven* trigger — bake the
  canonical image onto a larger EBS and confirm `df /boot` widens on the first boot
  with checkfs clean and the second boot a no-op (verification plan T4). Until that
  run lands, treat the loader trigger as compile-proven, not boot-proven.

Detection of an **online** EBS grow, a **hot-attached** second volume, and
**multi-controller** NVMe enumeration is out of scope here and tracked separately
(the kernel reads disk geometry once at boot); the shipping path is
launch-at-final-size + first-boot grow.

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
  with `num_blocks`. A large grow ADDS bitmap blocks and AGs.
- The bitmap sits at a FIXED offset (block 1). Extra bitmap blocks land at
  `[oldBitmapBlocks+1, newBitmapBlocks+1)`. In a stock layout that region holds
  the log and possibly data — **v2 eliminates that collision at format time**
  instead of resolving it at grow time.
- `disk_super_block` carries **no checksum**. A torn superblock write is
  undetectable garbage. This drives the intent-record design below.

## Part A — format-time change (mkbfs, canonical image only)

Bake the canonical image with growth headroom:

1. **Reserved bitmap gap.** Size the gap for the maximum supported grow target:
   `maxBitmapBlocks = ceil(maxNumBlocks / (block_size*8))` for a chosen
   `maxNumBlocks` cap (e.g. 1 TiB at 2 KiB blocks ⇒ ~32 MiB of bitmap; round the
   gap to 64 MiB for slack). Blocks `[oldBitmapBlocks+1, maxBitmapBlocks+1)` are
   left with no owner and **marked allocated in the bitmap at format time**, so
   nothing can land there before first boot.
2. **Log placed high.** The log block_run starts at `maxBitmapBlocks+1` instead
   of `oldBitmapBlocks+1`. Everything downstream (data area start) shifts
   accordingly. No BFS invariant depends on the log immediately following the
   bitmap — `log_blocks` is an ordinary block_run read from the superblock —
   but T1 must confirm stock Haiku/DeBeOS tools (`checkfs`, `bfs_shell`,
   bootloader) accept the layout.
3. **Cap recorded.** `maxNumBlocks` (the gap's design limit) is recorded in the
   superblock spare space so the grow can refuse targets beyond the baked
   headroom instead of silently falling into the relocation case.

With this bake, a grow to any target ≤ `maxNumBlocks` touches only:
bitmap blocks inside the pre-reserved (empty, already-allocated) gap, the old
last bitmap block's tail bits, and the superblock. **Nothing is relocated,
nothing owned by the old FS is overwritten.**

## Part B — execution point: mount-time, pre-cache

The grow runs inside `bfs_mount`, after the superblock is read and validated,
**before** `block_cache_create()` and before the journal is activated for
normal operation. Properties:

- Single-threaded, no concurrent writers, no transactions in flight — the
  "offline" safety posture without a separate boot phase, and it reconciles
  with the first-boot hook in T4 (the hook only needs to grow the *partition*;
  the FS grow happens on the next mount automatically).
- The block cache is created *after* the grow with the **new** `num_blocks`,
  so the new tail is fully cacheable. `block_cache_set_size` (v1 item 3) is
  unnecessary; the API sketch moves to the gated appendix with the online path.
- Trigger condition: partition size (from the partition layer / geometry ioctl)
  exceeds `num_blocks * block_size` by more than the item-1 threshold.
- I/O in this window is raw `read_pos`/`write_pos` on the device fd with
  explicit flushes (below) — not journaled, by construction idempotent.

## Part C — intent record

**Location:** a fixed, discoverable address *outside the old filesystem*:
the **last block of the grown partition** (`partitionBlocks - 1`, in old-FS
block units). Rationale:
- Writing it can never corrupt old-FS state (it's beyond old `num_blocks`).
- Recovery needs no pointer: whenever mount sees partition size > FS size
  (exactly the grow trigger), it probes that block for the magic before doing
  anything else. No second superblock write, no reliance on spare fields for
  addressing.
- After a completed grow the block lies inside the new FS — the grow marks it
  allocated in the new bitmap and the final step erases the magic and frees it.

**Contents** (single block, self-contained, CRC over the record):
- magic + version + CRC
- phase marker
- full old `disk_super_block` (verbatim copy)
- full target `disk_super_block` (new `num_blocks`, `num_ags`, `used_blocks`)
- target geometry summary (newBitmapBlocks, gap bounds) for cross-checks

Because the superblock has no checksum, the intent record is the *only* thing
that makes the commit repairable: with both superblock images stored, recovery
after a torn commit write can decide which geometry is consistent with the
bitmap state and rewrite block 0 whole. The commit is idempotent by replay.

## Part D — grow algorithm (fast path, the only shipping path)

Inputs: `newNumBlocks` = partition capacity (capped at `maxNumBlocks` from the
superblock; larger targets grow to the cap and log a warning).

1. **Classify / guard.**
   - `newBitmapBlocks == oldBitmapBlocks` → item 1 path (merged). Done here.
   - `newNumBlocks` ≤ old → refuse (no shrink).
   - New bitmap region not fully inside the baked gap (foreign volume, stock
     layout, or target beyond cap) → **refuse with `B_NOT_SUPPORTED`, FS
     untouched.** The relocating general case is not shipped (appendix).
   - Sanity: verify the gap region's bitmap bits are set (allocated-reserved,
     as baked) and no inode/log/data structure claims them. Any mismatch →
     refuse.
2. **Write intent record** (phase=STARTED) at `partitionBlocks - 1`. **FLUSH.**
3. **Write new bitmap blocks** into the gap `[oldBitmapBlocks+1,
   newBitmapBlocks+1)`: group-initialized (reuse
   `BlockAllocator::InitializeAndClearBitmap` logic), marking used = {the new
   bitmap blocks themselves, remaining reserved gap up to `maxBitmapBlocks+1`,
   the intent-record block}, free = everything else up to `newNumBlocks`;
   out-of-range convention bits set past `newNumBlocks` in the new last bitmap
   block. This region was baked-empty and baked-allocated: overwriting it
   destroys nothing and conflicts with no old-FS reader.
4. **Fix the old tail bits.** In the *old* last bitmap block, clear the
   out-of-range bits beyond old `num_blocks` (those addresses are now real,
   free blocks). This edits an existing bitmap block, but is safe on either
   side of commit: under old geometry those bits are past `num_blocks` and
   ignored by the allocator (last-AG cap at `num_blocks`); under new geometry
   they are correctly free. Stated explicitly so T2 proves it. **FLUSH** (all
   bitmap writes durable before commit).
5. **Update intent record** (phase=BITMAP_DONE). **FLUSH.**
6. **Atomic commit:** ONE `write_pos` of block 0 with the target superblock:
   `num_blocks`, `num_ags`, `used_blocks` (+= new bitmap blocks + remaining
   reserved gap + intent block; the old gap accounting already covered the gap,
   so recompute precisely from the bitmap), `log_*` unchanged (log never
   moves). **FLUSH.**
7. **Erase intent record** (zero the magic), free its block in the bitmap,
   decrement `used_blocks` accordingly via a second superblock write — or
   simpler and preferred: leave the block permanently allocated (one block of
   waste) and only zero the magic, keeping the commit single-write. **FLUSH.**
8. Proceed with normal mount: `block_cache_create(fd, newNumBlocks)`, journal
   replay (the log was never touched, so replay semantics are unchanged),
   allocator init picks up the new free space naturally — no
   `Reinitialize()` special case.

Note on the log: because the log is never moved and no transactions run in
this window, the v1 quiesce/drain problem does not exist on this path. Journal
replay ordering vs. grow: **intent-record detection and grow recovery run
before journal replay** in `bfs_mount` (the grow only touches regions the log
cannot reference — baked-gap + beyond-old-end — so either order is actually
safe; doing recovery first keeps the reasoning trivial).

## Part E — crash recovery

On every mount where partition size > FS size (or an intent probe is cheap
anyway), read `partitionBlocks - 1`:

- **No magic / bad CRC:** no grow in progress (or intent write itself was torn
  — indistinguishable and equally safe). Mount normally under the block-0
  superblock; if partition > FS, a fresh grow starts from step 1.
- **phase=STARTED:** bitmap writes may be partially applied — but only inside
  the baked gap and the old tail bits, none of which old geometry reads.
  Roll forward: redo steps 3–7 idempotently (all writes are absolute-content,
  not read-modify-write, except the old-tail-bit clear which is idempotent).
- **phase=BITMAP_DONE:** commit may or may not have landed, possibly torn.
  Compare block 0 against the two superblock images in the intent record:
  matches old → redo 6–7; matches new → redo 7; matches neither (torn write) →
  rewrite block 0 with the target image (bitmap is already fully in target
  state per phase), then 7. Never mount through a superblock that matches
  neither image.

There is **no rollback path** and none is needed: before commit, old geometry
never references anything the grow wrote; after intent=STARTED every step is
roll-forward idempotent. This also eliminates v1's reservation-leak problem —
the fast path reserves nothing from free space.

## Part F — write-ordering discipline

Every **FLUSH** above is a full device flush (FUA/flush ioctl on the raw fd).
Required barriers: after intent write, after all bitmap writes, after commit,
after intent erase. The verification harness must test not only phase-boundary
aborts but arbitrary persisted-write subsets between barriers (see plan deltas).

## Scope guard
Shipping surface = Part A (mkbfs) + Parts B–F (mount-time fast path). The
relocating general grow (v1 steps 2–6: reserve/evacuate/log-relocate) and the
online grow + `block_cache_set_size` are moved to APPENDIX-GENERAL-GROW as a
design record, permanently behind `B_NOT_SUPPORTED`, with the additional
refusal rule inherited here: any non-data occupant (inode, b+tree node,
indirect block) in a to-be-claimed region is grounds for refusal, because
inode block addresses are inode numbers and relocation would cascade into
directory b+trees and index structures.

---

## Verification plan deltas (v2)

The tiered plan stands (T0–T4) with these changes:

- **T1 additions:** stock-tool acceptance of the Part-A layout (log-high +
  allocated gap): `checkfs` clean on the *baked, ungrown* image; boots; item-1
  grow still works on it. Content-checksum manifest comparison is required in
  T1 **and T2**, not just `checkfs` — structural checks can't catch data landing
  in a wrong-but-valid location.
- **T2 redefinition:** abort points are now {after intent, mid-bitmap-writes,
  after bitmap flush, after phase update, mid-commit (torn block 0 — synthesize
  a half-old/half-new block 0), after commit, after intent erase}. Additionally,
  **write-subset replay**: run the grow host-side in `bfs_shell` against a raw
  image behind a write-logging shim; replay arbitrary prefixes and legal
  subsets (writes between two flush barriers may persist in any subset/order;
  flushes are ordering fences) and mount-check every state. Thousands of crash
  states host-side; real-Graviton runs (EBS snapshot/restore) remain the final
  confirmation tier, not the exploration vehicle.
- **T2 pass criterion unchanged:** every state mounts as either old-geometry
  or completed-new-geometry, `checkfs` clean, manifest intact. Zero tolerance.
- **T3 (online) removed** from the acceptance path (no online grow shipped);
  the section moves to the appendix with its feature.
- **New T1.5 — refusal paths:** grow on a stock-layout (no gap) volume →
  `B_NOT_SUPPORTED`, FS untouched, still mounts; target beyond `maxNumBlocks`
  → grows to cap; shrink refused; gap-sanity mismatch (deliberately corrupt a
  gap bit) → refuse; double grow 19→25→40 with recovery injected in each;
  boundary target that adds exactly one bitmap block (item-1/item-2
  classification edge); non-AG-aligned target (partial last AG, `num_ags`
  invariant holds).
- **T4 unchanged** except the first-boot hook's responsibility shrinks to the
  GPT/partition grow; the FS grow is implicit in the next mount.
