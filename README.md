# nevtoraw

Extracts frames of Nikon **N-RAW** video (`.NEV`, e.g. Z 8) as raw stills.

```
nevtoraw [dng] <file.NEV> [-f 0 | 0-10 | 0,12,50 | 0-143:6 | all] [-o DIR] [-c ljpeg|none] [--white N]
nevtoraw stack <file.NEV> [-f 0-15] [--merge mean|median] [-r REF] [--no-align] [--tile 64] [-o DIR] [-c ljpeg|none]
nevtoraw info  <file.NEV>
```

`dng` is the default, so `nevtoraw clip.NEV -f all -o out` works. Without `-f` only frame 0 is
exported. `stack` averages frames (default 0-15) into one low-noise DNG.

Build: `mingw32-make` (MSYS2 MinGW-w64 g++, C++17), producing one static `nevtoraw.exe`.
The HE/TicoRAW decoder in `third_party/nikon-he-public` and `src/he_decode.cpp` come from
[nefcomp](https://github.com/missingno7/nefcomp).

## Findings (Z 8 FW 4.06)

**Tested modes.** All are 12-bit N-RAW, decoded to a 14-bit RGGB Bayer mosaic with black 1008.

| clip | mode | coded | output (DefaultCrop) | sensor area (CropHiSpeed) | bpp | sampling |
| --- | --- | --- | --- | --- | --- | --- |
| DSC_7345 | 8.3K 59.94p | 8268×4652 | 8256×4644 | `6`: 8280×4656 at row 432 (FX width, 16:9) | 2.50 | 1:1 |
| DSC_7346 | 4.1K 119.88p | 4140×2330 | 4128×2322 | `6`: 8280×4656 at row 432 (same FX band) | 3.00 | ½ in each direction, still a clean Bayer pattern |
| DSC_7347 | UHD 119.88p | 3852×2168 | 3840×2160 | `20`: 3856×2176 at (2212, 1672) | 3.00 | 1:1 centre crop (≈2.15×) |

The 4.1K mode covers the full FX field at half resolution: the sensor bins or scales same-colour
pixels, and the result is a regular RGGB mosaic (G1/G2 balanced, no CFA artefacts). UHD 120p is a
native-resolution centre crop. Each makes a valid DNG.

**Container.** ISO-BMFF/MOV (`ftyp niko`). Video track sample entry `NRAW` with the coded size,
`stts`/`mdhd` for the frame rate, one frame per chunk (`co64`). `udta/NCDT` holds `NCTG` (movie tags:
model, firmware, date, fps, output size at 0x22/0x23, lens, serial, shutter count 0x20000a7,
makernote-style tags at 0x2000xxx), `NCTH`/`NCVW`/`NCM1` JPEGs.

**Sensor area.** Makernote CropHiSpeed (NCTG 0x200001b) is `mode, sensor w, sensor h, area w,
area h, left, top`. For 8.3K and 4.1K it is `6, 8280, 5520, 8280, 4656, 0, 432`, a 16:9 band of the
full sensor width. The coded frame is the area (or half of it) minus a 6/2 px border on each side.

**Frame sample** (constant size per clip; 12,020,736 bytes for 8.3K):

| offset | content |
| --- | --- |
| 0x000 | `NRAW` box: size, `NRFM` "0100" |
| 0x014 | `NRFH` → `NRHM` "0100" + `NRMT` items: `group(2) tag(2) type(1) value(BE)` |
| 0x343 | `NRTH` → `NRTI` (codestream size) + `NRTA` padding up to 0x400 |
| 0x400 | TicoRAW codestream (`FF10` … `FF11`) |

`NRMT` groups: `0x0110` Exif-like (width, height, bits=14, orientation, ExposureTime and FNumber as
float32, ISO, bias, focal length, CFA RGGB); `0x0190` Nikon private (`0x0009` WB R/B gains as
float32; `0x0010` colour temperature; `0x0012` 3×3 float matrix, white-balanced camera RGB → XYZ
D65, rows sum to the D65 white); `0x0200` a makernote fragment (inline entries: `0x000c` WB
levels, `0x003d` black 1008, `0x0045` crop 6,4,8256,4644, `0x0058` codestream size, `0x0093`=15,
`0x0097` encrypted ColorBalance). Exposure and WB are per frame and change during auto exposure.

**Codec.** It's the same intoPIX TicoRAW as the stills' HE/HE★ NEFs. The picture header is
byte-identical to a Z 8 HE NEF apart from Lcod/width/height (same CDT/WGT, same companding
extension with black 1008). The "12-bit" N-RAW decodes to the 14-bit scale. 8.3K uses 2.50 bpp,
below both HE (3 bpp) and HE★ (5 bpp); the 120p modes use 3.00. Decoding takes about 0.65 s per
8.3K frame and 0.16 s per 4.1K/UHD frame.

**Decoder fix.** The bundled decoder's sub-band layout (`nikon_he_subband_config.cpp`) estimated
the 5-level wavelet band sizes from the level-1 group count. That is exact for the stills widths and
for 8268/4140, but off by one group at 3852 (UHD), so UHD frames failed to decode. It now derives
each band from the halving the 5/3 lift actually performs. 8.3K and 4.1K output is bit-identical to
before.

## Outputs

### DNG (implemented)
Uncompressed or tiled lossless JPEG (Compression 7, 256×256, 2-component CFA trick). Each file
carries Adobe's Z 8 ColorMatrix1/2 (from DNG Converter 17.2), AsShotNeutral from the frame's WB,
BlackLevel from the codestream, DefaultCrop (6,4) + the camera's output size, FrameRate, Exif
(per-frame exposure, ISO, lens, date + offset, SubSecTime) and the original file name.

* dcraw decodes both variants bit-identical to the decoder output (frames across all three clips).
* Adobe DNG Converter 17.2 accepts all three modes, its raw data is identical, and it applies the
  output crop (8256×4644, 4128×2322, 3840×2160). The 8.3K render has correct colour.
* Size (lossless JPEG): 46.7 MB per 8.3K frame (noise at ISO 10000 limits compression, against a
  12 MB source frame), 11.7 MB per 4.1K frame and 11.2 MB per UHD frame.
* Other cameras: the matrices fall back to the frame's own matrix (single illuminant), which is
  untested.
* White level 16383 is unverified: this clip has no clipped highlights.

### Stacked DNG (`stack`)
Aligns a burst of frames and averages them into one DNG that is still a raw mosaic, so Camera Raw
demosaics it and its profiles and Denoise still apply. It follows the HDR+ approach and never
demosaics:

1. **Reference:** the middle frame of the list (`-r` to choose). Every frame is split into its four
   CFA planes, black-subtracted and scaled to the reference exposure (ExposureTime × ISO / f²).
2. **Align:** on a 2×2-binned grey image, with 64×64-sensor-px tiles and a 4-level pyramid. The
   camera motion is found first as a whole-image translation (±16 px at the coarsest level), then
   each level refines every tile by ±2 px, with a parabolic sub-pixel fit at the finest. After
   every level, tiles that disagree with their 5×5 neighbourhood median are replaced by it, and an
   affine model of the camera motion (translation, rotation, scale, shear, which covers most
   rolling-shutter skew) is fitted robustly (IRLS, Tukey). Tiles more than 1.5 px from that model
   are set to it, so a moving object cannot drag the background with it. The tile field is
   interpolated bilinearly per pixel.
3. **Warp:** each CFA plane is resampled separately (Catmull-Rom), so the colour pattern is kept.
4. **Merge** (`--merge`):
   * `mean` (default): a robust weight per 2×2 quad compares the warped frame with the reference
     in a 3×3 low-passed grey image. The expected difference comes from a noise model
     (`σ² = A·s + B`) fitted on the first frame pair by reweighted least squares that drops
     brightness bins dominated by mismatches. The weight is 1 below 3σ and 0 above 5σ, then a
     3×3 minimum so motion edges drop out as a whole. A moving object is kept as it is in the
     reference frame (sharp, single-frame noise) and is removed from all other frames: no ghosts.
   * `median`: per-sample median of every aligned frame that covers the sample. Anything present
     in fewer than half the frames disappears, including from the reference (people walking
     through, cars). It needs every frame in memory (2 bytes per sensor pixel each).

   Clipped samples are never merged.
5. **Output:** the result ×4 as 16-bit (BlackLevel 4032, WhiteLevel 65532), which keeps two extra
   bits. ImageDescription records the mode, the frames and the reference. Files are named
   `_stack_` (mean) or `_median_`.

Measured on the 4.1K clip (G plane, flat areas, high-pass noise; the single frame here is 40.5 DN):

| frames | mean | median | ideal 1/√N |
| --- | --- | --- | --- |
| 16 | 11.1 | 12.5 | 10.1 |
| 64 | 6.6 | 7.3 | 5.1 |

* **Noise:** median costs only 10–15 % more noise than mean (not the 25 % expected for Gaussian
  noise), and shows no level bias (under 1 DN in shadows and highlights).
* **Sharpness:** the unaligned 64-frame average smears small text and barcodes (about 10 px of
  drift). Aligned stacks keep the single frame's edge contrast (0.99–1.00) in both modes. On the
  8.3K clip, 16 frames spread over 2.3 s (up to 16 px handheld drift) align cleanly.
* **Moving object:** these clips have none, so a test build painted a 240 px block moving 200 px
  per frame into 16 frames. `mean` keeps it only at the reference position and is clean elsewhere.
  `median` removes it entirely and matches the clean stack within noise. Neither warps the
  background. Before the regularised alignment and robust noise fit, the block bent the
  background around it and inflated the noise model about 1100×, so both modes failed.
* **Adobe:** DNG Converter reads the stacked DNG bit-identically.
* **Cost** (16 frames): 4.1K about 3.5 s; 8.3K about 12.5 s, with 1.2 GB peak (mean) or 2.3 GB
  (median, plus 77 MB per extra frame).

### NEF (not pursued)
A frame's codestream placed unchanged into a Z 8 HE NEF opens in Adobe DNG Converter and decodes
bit-identically, which independently confirms the 8.3K decode. Adobe gives such NEFs a centred
DX-sized crop (5392×3592) whatever the makernote says, and rejects the smaller 4.1K/UHD sizes,
so DNG is the only output.

## Credits / licences

* `third_party/nikon-he-public`: public HE decoder from LibRaw PR #826 (Dmitri Sotnikov, with
  z8-qemu fixes), LGPL-2.1 / CDDL.
* `src/he_decode.cpp`: bridge from [nefcomp](https://github.com/missingno7/nefcomp), adapted from
  [WonderLantern z8-qemu](https://github.com/WonderLantern/z8-qemu) (GPL-2.0-or-later).
* This program is therefore GPL-2.0-or-later (see `COPYING`). Nikon, Adobe and intoPIX own their
  trademarks and patents. This is an independent interoperability tool for your own files.
