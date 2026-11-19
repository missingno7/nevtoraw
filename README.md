# nevtoraw

Extracts frames of Nikon **N-RAW** video (`.NEV`, e.g. Z 8) as raw stills.

```
nevtoraw info <file.NEV>
nevtoraw dng  <file.NEV> [-f 0 | 0-10 | 0,12,50 | 0-143:6 | all] [-o DIR] [-c ljpeg|none] [--white N]
```

Build: `mingw32-make` (MSYS2 MinGW-w64 g++, C++17), producing one static `nevtoraw.exe`.
The HE/TicoRAW decoder in `third_party/nikon-he-public` and `src/he_decode.cpp` come from
[nefcomp](https://github.com/missingno7/nefcomp).

## Findings (Z 8 FW 4.06, `examples/DSC_7345.NEV`)

**Container.** ISO-BMFF/MOV (`ftyp niko`). Video track sample entry `NRAW`, 8268×4652,
59.94 fps (`stts` 1001 @ 60000), one frame per chunk (`co64`). `udta/NCDT` holds `NCTG` (movie tags:
model, firmware, date, fps, output size 8256×4644 at 0x22/0x23, lens, serial, shutter count
0x20000a7, makernote-style tags at 0x2000xxx), `NCTH`/`NCVW`/`NCM1` JPEGs.

**Sensor area.** Makernote CropHiSpeed (NCTG 0x200001b) = `6, 8280, 5520, 8280, 4656, 0, 432`:
a 16:9 crop that keeps the full FX width (8280 px) and reads rows 432…5087 of the 5520-row sensor.
The coded frame (8268×4652) is that band minus a 6/2 px border, so it's FX width, not DX.

**Frame sample** (12,020,736 bytes each, constant):

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
extension with black 1008). The "12-bit" N-RAW decodes to the 14-bit scale. At 2.50 bpp it is
below both HE (3 bpp) and HE★ (5 bpp). nefcomp's bundled decoder handles it unchanged, about
0.65 s per frame.

## Outputs

### DNG (implemented)
Uncompressed or tiled lossless JPEG (Compression 7, 256×256, 2-component CFA trick). Each file
carries Adobe's Z 8 ColorMatrix1/2 (from DNG Converter 17.2), AsShotNeutral from the frame's WB,
BlackLevel from the codestream, DefaultCrop (6,4)+8256×4644, FrameRate, Exif (per-frame exposure,
ISO, lens, date + offset, SubSecTime) and the original file name.

* dcraw decodes both variants bit-identical to the decoder output.
* Adobe DNG Converter 17.2 accepts them, its raw data is identical, it renders correct colour and
  it applies the 8256×4644 crop.
* Size: 46.7 MB/frame lossless (noise at ISO 10000 limits compression), against a 12 MB source frame.
* Other cameras: the matrices fall back to the frame's own matrix (single illuminant), which is
  untested.
* White level 16383 is unverified: this clip has no clipped highlights.

### NEF (experiment, not implemented)
The frame's codestream can go unchanged into a Z 8 HE NEF container, which keeps the data
bit-exact at about 17 MB. In the test, the template was a camera HE NEF with patched raw-IFD
dimensions, StripByteCounts, makernote 0x0058 and 0xC7D5/0x000F (with CRC). Adobe DNG Converter
opens it and decodes it **bit-identically**.

**But** Adobe applies a centred DX-sized default crop (5392×3592) for this raw size. Changing
makernote 0x0045 (CropArea) or 0x001b (CropHiSpeed) has no effect, so it is probably Adobe's
own size table. The template's previews, WB (0x000c/0x0097), shutter count and Exif would also
have to be replaced per frame. NX Studio, LibRaw and darktable were not tested.

## Credits / licences

* `third_party/nikon-he-public`: public HE decoder from LibRaw PR #826 (Dmitri Sotnikov, with
  z8-qemu fixes), LGPL-2.1 / CDDL.
* `src/he_decode.cpp`: bridge from [nefcomp](https://github.com/missingno7/nefcomp), adapted from
  [WonderLantern z8-qemu](https://github.com/WonderLantern/z8-qemu) (GPL-2.0-or-later).
* This program is therefore GPL-2.0-or-later (see `COPYING`). Nikon, Adobe and intoPIX own their
  trademarks and patents. This is an independent interoperability tool for your own files.
