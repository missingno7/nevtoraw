# Execution and memory

Conversion, stacking, SR, and DNG encoding share a persistent CPU worker pool and
one memory allowance. No tuning is required for normal operation. The program
prints the selected worker count and budget at startup, then reserved-memory and
stage-time statistics at completion. Stage work times add across overlapping jobs;
they need not add up to elapsed wall time or CPU time.

## Defaults and overrides

The automatic budget is 70% of available physical RAM, bounded by available
commit headroom on Windows. It is measured at command startup, leaving the rest
for other applications and the OS. `--memory-limit auto` selects the same policy.
The worker limit defaults to the logical CPU count. Expensive jobs are admitted
according to their estimated working memory; a large worker limit does not force
all workers to allocate full-frame buffers simultaneously.

```powershell
# Automatic workers, RAM allowance, and disk backing.
.\nevtoraw.exe sr clip.NEV --count 256

# Optional constraints; these settings also work with dng and stack.
.\nevtoraw.exe stack burst --merge trimmed-mean --memory-limit 8G --threads 8
.\nevtoraw.exe dng clip.NEV --frames 0-31 --temp-dir D:\scratch
```

Sizes accept positive integers with B/K/M/G, KB/MB/GB, or KiB/MiB/GiB suffixes;
all units are binary. The temporary directory must already exist.

## Processing pipeline

After preparing the reference, bounded prefetch overlaps reading/decoding upcoming
frames with alignment and merging. NEV sample reads synchronize the shared file
position, then independent decoders can run concurrently. HE lookup initialization
is thread-safe and mutable decoder layout/header state is local to each thread.
Frames are consumed in selection order, preserving merge ordering and metadata.

Workers reuse one pool for image rows, alignment searches, reconstruction tiles,
and compression. Nested parallel work runs locally to avoid worker starvation
and oversubscription. Exceptions from workers are propagated to the caller; queued
reads finish before their input objects are destroyed.

Mean stacking uses incremental accumulators. Median/trimmed mean retain aligned
sample arrays in the shared cache and merge bounded spatial chunks across the
entire accepted selection. They do not merge medians of frame groups. Ordinary
12/14-bit samples retain the previous half-DN representation; 16-bit/high-black
inputs retain float samples.

SR retains the reference and caches other original CFA arrays. Each output tile
reads only its transformed source regions, with the kernel's required margin.
Small outputs use smaller tiles; tile selection depends on image geometry rather
than worker count. Changing the tile boundaries from earlier versions can change
floating-point rounding by a small amount; merge equations and kernel are unchanged.
If a full SR output buffer does not fit, reconstructed tiles use the same cache.

## Cache and output lifecycle

Working buffers and bounded decode queues reserve memory before caches admit
arrays. Cache entries stay in RAM while allowance remains; later entries are
written to private scratch files. Disk-backed arrays and output tiles contain
the same sample values as their resident equivalents. Temporary files are closed
and removed on success or exceptions; Windows also removes open scratch files
when the process exits. Disk errors stop processing and report the failure.

The DNG writer compresses bounded jobs, caches their encoded payloads, then writes
the TIFF header and payloads to the destination's temporary file. It does not build
a second complete DNG byte array. Replacement happens only after a successful
close, preserving an existing output if encoding or writing fails.

## Practical limits

The allowance governs managed caches plus conservative estimates for working
buffers. It is a scheduling target, not an OS-enforced process RSS ceiling.
Allocator overhead, thread stacks, source metadata, and the OS file cache are
additional. The first frame is decoded to discover geometry before the full
working-memory plan is established. Input decoders still need full-frame working
buffers even for a small crop. A budget below that minimum produces an error
instead of trying to run the whole operation through tiny tiles.

A stack crop is still extracted after full-frame merging. SR crops reduce output
work but do not eliminate full-frame decoding/alignment. Disk backing trades I/O
for RAM, so a tight limit can reduce throughput. The DNG 4 GiB classic-TIFF limit
is independent of the memory budget and remains enforced before SR reconstruction.
