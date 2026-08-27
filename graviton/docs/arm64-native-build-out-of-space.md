# Parallel native Rust builds: out of space, not filesystem corruption

## Symptom

Large **parallel** native builds (a 200+-crate Rust project) on the arm64 image
failed in ways that looked like data corruption:

- A file that had just been written came back missing on the next open —
  `B_ENTRY_NOT_FOUND` — e.g. a compiler output `.rcgu.o`, or `ar` reporting
  `No such file or directory` for the archive it was creating.
- Occasionally the compiler crashed decoding a dependency's metadata.

`cargo build -j1` (serial) appeared to work; `-jN` (parallel) failed. The natural
hypothesis was a BFS or DMA data-corruption bug under concurrency.

## What it actually was

Two independent things, **neither of them corruption**:

1. **Out of space — the dominant cause.** The image was an 8.8 GiB BFS partition
   already ~91% full. A parallel debug build's burst of concurrent intermediate
   files overran the remaining headroom, and ENOSPC on BFS surfaces as "vanished"
   files (`B_ENTRY_NOT_FOUND`). A serial build has a far smaller *peak* on-disk
   footprint, so it slipped under the limit — which is the whole of the
   "`-j1` works, `-jN` fails" pattern, with no filesystem defect involved.

2. **An upstream `rustc` ICE — secondary and unrelated.** A specific crate
   version crashes `rustc`'s metadata decoder (`rmeta/decoder.rs`)
   *deterministically*, reproducible at `-j1` with many gigabytes of free disk —
   so it is a compiler bug, not corruption or concurrency. Observed with
   `darling_macro` 0.24.1; `darling` 0.20.11 (used by the real project) compiles
   fine.

## How it was distinguished

A small concurrent-write instrument — write a self-describing pattern from many
threads, read it back, and classify every mismatch as *zero* / *cross-file* /
*garbage* — found **zero** corruption across every pattern tried on a disk with
room to spare:

- cached writes, read back;
- **pure `O_DIRECT`** writes *and* reads (the file cache bypassed on both ends,
  i.e. the raw device/DMA path);
- write-to-temp + `rename` churn over thousands of small files;
- all of the above under heavy memory pressure.

The purest raw-DMA path being clean rules out a DMA / memory-ordering
data-corruption cause. Then **squeezing free space to a sliver reproduced the
exact `B_ENTRY_NOT_FOUND` errno** on freshly written files — pinning the failure
to ENOSPC.

## Fix

Size the image for real development: a 20 GiB root volume with an ~18.6 GiB BFS
partition (`HAIKU_IMAGE_SIZE=19000` in the cross-build spec, `rootVolumeBytes`
= 20 GiB in the pipeline config). Verified by building the real 200+-crate
project **in parallel** to a linked binary on the same instance type that
previously hit ENOSPC — it finished with ~12.5 GiB free to spare.

Note: the BFS root partition does **not** auto-grow to a larger EBS volume; the
partition size is fixed at bake time (`HAIKU_IMAGE_SIZE`). Growing a mounted BFS
root is unsupported, so oversizing the volume alone would only waste slack —
both the volume and the partition must be sized together.

## Note on the NVMe barrier

An NVMe submit-doorbell store barrier was widened to outer-shareable (`dsb oshst`)
while this was under investigation. It is defensible as hardware hygiene, but it
was **not** the fix here — the failures were out of space, not a memory-ordering
data-corruption bug.
