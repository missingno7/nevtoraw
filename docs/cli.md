# Command-line reference

```text
nevtoraw [dng] INPUT [OPTIONS]
nevtoraw stack INPUT [OPTIONS]
nevtoraw sr INPUT [OPTIONS]
nevtoraw info INPUT
nevtoraw cameras
nevtoraw --version
```

`INPUT` is a NEV, NEF, NRW, Bayer DNG, or still-image folder. `dng` is the default
command. Keep one camera/burst and one source format in a folder. NEF and NRW are
both native Nikon input; mixing either with DNG is rejected. Indexing is
lexicographic filename order, without recursion.
Use `info INPUT` to inspect a folder's mapping. Folder metadata describes its
first file; compatibility of selected frames is checked during processing.

`cameras` lists the bundled Nikon RGB calibrations without opening an input.
It accepts no input or processing options. See the [camera catalog](nikon-cameras.md)
and [capture-format limits](raw-formats.md).

## Shared options

| Option | Meaning |
| --- | --- |
| `-f, --frames LIST` | Zero-based candidates. Default `0` for `dng`, `all` for `stack`/`sr`. |
| `-o, --output DIR` | Destination directory. Files default beside the source, folders to `SOURCE/nevtoraw`. |
| `-c, --compression ljpeg\|none` | Lossless JPEG (default) or uncompressed 16-bit output. |
| `--white N` | Override white level, integer 1–65535, above source black levels. NEV defaults to 16383. |
| `--black LEVELS` | Source black level in DN, replacing the camera's: `N` for all colours, `R,G,B`, or `R,Gr,Gb,B` (Gr is the green on red rows). Fractions allowed. `stack` and `sr` also accept `auto` and `pixel`. |
| `--dark PATH` | Measure per-colour black levels from dark frames: a folder, file, or NEV clip. Cannot accompany `--black`. |
| `--crop X,Y,W,H` | Physical output region in coded source/reference sensor pixels, before orientation and SR scaling. Minimum 2×2. |
| `--memory-limit SIZE` | Automatic by default: 70% of available physical memory/commit headroom. Override with an integer size such as `8G`, `512M`, or `auto`; suffixes use powers of 1024. |
| `--threads N` | Shared worker limit, 1–1024. Default logical CPU count. Memory availability can reduce active jobs. |
| `--temp-dir DIR` | Existing directory for automatically deleted disk caches; default OS temporary directory. |
| `-h, --help` | Show help, including after a command. |
| `--version` | Print the program version. |

Selections accept `all`, `0`, `10-25`, `0,12,50`, and `10-160:10`.
Ranges are inclusive. Endpoints beyond the input are clipped; individual indices
outside the input are errors. Duplicates are removed and indices sorted. Descending
ranges, zero steps, trailing commas, negative indices, and malformed values are errors.

`dng folder --frames all` exports every still. By default `dng` exports only index
0, including for clips and folders. Conversion preserves decoded raw samples and
levels; Nikon lossy NEFs are already linearized.

Cropping is available in `dng`, `stack`, and `sr`. With no crop, source default
crop metadata is preserved. An explicit crop extracts that exact coded rectangle,
rebases the output to `(0,0)`, and adjusts Bayer pattern/black-level phase for odd
origins. The original orientation is retained. Conversion preserves sample values.
Stacking aligns and merges full frames before extracting the crop, so its alignment
and memory requirements are unchanged. SR reconstructs only the requested output
region, retaining full-frame information for alignment. All coordinates and sizes
are in input pixels; `--scale` multiplies the SR output dimensions afterward.

## Black levels and dark frames

Cameras record one black level per frame or per colour, and small errors in it
are invisible in single frames. A stack removes most of the noise, so a colour
whose true black is a fraction of a DN below the tag falls below black in the
deepest shadows; raw converters clip it there and tint the darkest tones.

`--dark PATH` reads every frame in PATH (same folder, file, and NEV rules as the
input) and uses the robust mean of each colour as its black level: median ±6
robust standard deviations, so hot pixels are ignored. Shoot 8–16 frames with the
lens cap on at the same ISO and shutter speed, ideally right after the burst.
The dark frames must have the input's coded size; a different camera model or ISO
prints a warning. The measured levels, noise, and ignored samples are printed.

`--black` sets levels directly, for example `--black 1008,1008,1007.4` for R, G,
and B. Levels replace the source black level and per-CFA offsets for every frame.
Conversion writes them as per-site DNG black levels; stacking and SR subtract them
before merging and record them in the DNG description.

`stack --black auto` normalizes the merged result instead: for each CFA site it
takes the 64×64 px block means of the whole stacked frame and shifts the site so
its darkest blocks average exactly zero. A low order statistic (the fifth-darkest
block for a typical full frame) ignores a few defective blocks. Single pixels are
not used because, after averaging, the darkest pixel is still mostly noise. This
assumes the scene contains areas that receive essentially no light, such as night
shadows; in a scene without them it would remove real signal. The chosen levels
are printed and recorded in the DNG description. It cannot accompany `--dark` or
numeric `--black` levels.

`stack --black pixel` applies the same normalization with the single darkest
merged sample of each CFA site, for comparison. It includes noise and any defect:
in an aligned burst the darkest samples are usually at the frame edges, covered
by fewer frames, so shadows are lifted by several DN.

`sr --black auto|pixel` applies the same rules to the reconstructed output, with
blocks of 64×64 input pixels (64 × `--scale` output pixels). SR measures only the
region it reconstructs, so with a small `--crop` the darkest part of the crop may
not be black; use levels from a full stack (`--black R,Gr,Gb,B`) instead. SR writes
the result as fractional DNG black levels, per channel for LinearRaw and per CFA
site for `--bayer`, so the samples are not requantized. Both greens share one
estimate in SR.

## Frame count and reference (`stack` and `sr`)

| Option | Meaning |
| --- | --- |
| `--count N\|all` | N evenly spaced candidates, including the reference. Default all for `stack`, min(16, candidates) for `sr`. |
| `-r, --ref N` | Reference index in the original input. Default middle of the final selection. |

`--frames 10-50 --count 10` selects
`10,14,19,23,28,32,37,41,46,50`. `--frames 10-25 --count all` selects
16 consecutive frames. `--frames 10-160:10 --count all` selects 16 spaced frames.
No sharpness ranking or automatic replacement is performed.

An explicit reference must be a candidate. If absent from the evenly spaced
selection, it replaces the nearest sampled index, preserving the count.
`--count 1` selects the middle candidate or requested reference. SR still
reconstructs the requested scale/format; use `dng` for original Bayer samples.
An explicit count exceeding the candidates is an error. The experimental
`--counts` spelling is no longer accepted; run once per desired count.

## Stacking

| Option | Meaning / default |
| --- | --- |
| `--align homography\|tiles` | Global perspective warp (default) or regularized local tile field. |
| `--no-align` | Identity alignment for tripod captures. Cannot accompany `--align` or `--tile`. |
| `--tile N` | Even 16–512 sensor pixels, default 64. Alignment guide grid size. |
| `--merge mean\|median\|trimmed-mean` | Default robust `mean`. |
| `--trim-percent N` | Integer 0–49 per tail, default 20. Requires `--merge trimmed-mean`. |

Homography estimates translation, rotation, scale, and perspective from control
points. The tile alternative handles some local differences; parallax and moving
subjects can still cause artifacts. `mean` uses a noise-based robust weight and
retains moving subjects from the reference. `median` rejects stronger outliers.
`trimmed-mean` drops `floor(N * percent / 100)` samples from each tail at each
covered site and averages the rest. Small sets may trim zero samples.
Clipped source footprints do not contribute.

Read/format/compatibility failures stop stacking. Frames lacking a reliable
homography are reported and skipped; failure to align any other selected frame
is an error. Output metadata records the frames actually accepted.

Mean stacking streams frames. Median/trimmed mean cache aligned samples, roughly
`width × height × accepted frames × 2 bytes`. 16-bit inputs or high black offsets
use 4-byte storage. This cache shares the automatic RAM budget and spills to disk
as needed. The merge reads bounded spatial chunks across all accepted frames,
preserving the per-pixel median/trimmed mean. Working-memory minimums still scale
with sensor dimensions. See [execution and memory](runtime.md).

Output scales values by `floor(65535 / reference_white)`, retaining fractional
averages in 16 bits where source headroom permits. This is ×4 for 14-bit sources
and ×16 for 12-bit sources. A 16-bit source may already occupy the available range.
Black offsets are subtracted per CFA site before merging.

## Super-resolution

| Option | Meaning / default |
| --- | --- |
| `--scale 1\|2\|3\|4` | Magnification per axis, default 2. |
| `--kernel-sigma N` | Finite positive Gaussian sigma in input sensor pixels, default 0.30. |
| `--bayer` | Mosaic for later demosaicing; default camera-native RGB LinearRaw. |

The reconstruction uses original CFA samples at transformed positions, with a
local linear fit and fixed 1.20-input-pixel search radius. Sigma does not change
with scale. `0.42` trades detail for smoothing; `0.20`/`0.10` emphasize nearby
samples and need more subpixel coverage. Very small sigmas can leave sparse areas
dependent on interpolation from the reference. Scale 1 is useful for native-size
linear raw; 2 is the general default; 3/4 suit detailed scenes and longer bursts.

SR caches selected raw frames, roughly 2 bytes per source pixel per frame,
using the shared RAM budget and temporary storage. Reconstruction loads only the
source regions needed by an output tile. Large reconstructed outputs are also
cached in tiles when a complete output buffer would exceed the budget. Small
crops use smaller jobs to improve CPU utilization. LinearRaw has three channels;
classic TIFF/DNG output remains limited to 4 GiB. Crop reduces output/reconstruction
work; full sensor decoding/alignment still requires working memory.

SR stops on source, compatibility, or homography failures, reporting the affected
frame. It does not substitute frames. Subject motion and saturated samples have
no separate robust rejection pass; use mostly static scenes.

## Output names and errors

| Command | Filename |
| --- | --- |
| NEV conversion | `clip_00000.dng` |
| NEF conversion | `photo.dng` |
| DNG conversion | `photo_converted.dng` |
| Stack | `reference_stack_00000-00016.dng`, `_median_…`, or `_trimmed20_…` |
| SR | `reference_sr2x_linear_16.dng` or `reference_sr2x_bayer_16.dng` |

For NEV merges, the stem is the clip name; for still bursts, the reference filename.
Stack filename indices give the selected interval; `ImageDescription` records the
exact accepted list. SR descriptions include selection, count, reference, scale,
and sigma. An explicit crop is recorded by all three processing commands.
The Software tag carries the program version.

A temporary file in the destination directory replaces an existing generated
output only after a successful write. Source frames are protected from replacement.
Failed writes remove their temporary file. Generated filenames may be replaced
by a later run with different options; use distinct `-o` folders for comparisons.

Exit code 0 means success, 2 invalid CLI options/selection, and 1 a read, format,
alignment, reconstruction, or write failure. Batch conversion may leave successful
exports when another frame fails; it returns 1.
