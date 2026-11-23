# Camera and raw formats

## Nikon input

Native decoding reuses the HE/HE★ bridge, Nikon lossless/lossy Huffman decoder,
and packing layouts from [nefcomp](https://github.com/missingno7/nefcomp).
Decoders are compiled into the executable; no external converter is invoked.

NEV supports the tested N-RAW container and HE/TicoRAW frame variants. Z 8 8.3K,
4.1K, and UHD modes were tested. Nominal 12-bit N-RAW decodes onto a 14-bit Bayer
scale. Other NEV bodies can supply their frame matrix when no bundled calibration
exists; that fallback has less validation.

NEF/NRW supports RGB Bayer 12/14-bit lossless/lossy compression, HE/HE★, and supported
packed/unpacked 12/14/16-bit layouts. Raw strips must be contiguous. Small-size RGB
NEFs and CMYG sensors lack the required Bayer mosaic. Nikon black-level tags are
converted from their 14-bit convention for 12-bit files. Coolpix NRW also accepts
UNDEFINED CFA bytes and reads its separate black-level blocks. White balance
handles the older D100/D2H/D70 ColorBalance records, E-series blocks, and NRW
records in addition to the usual Nikon WB tag. The two green multipliers are
averaged when normalizing white balance. Encrypted ColorBalance-only variants
are not decoded; a missing supported WB record leaves AsShotNeutral unspecified.
D1X output carries its non-square sensor aspect in DNG DefaultScale; alignment
and crop coordinates remain in the original sensor grid.

The bundled [catalog lists 98 RGB Nikon calibrations](nikon-cameras.md), also shown
by `nevtoraw cameras`. Model matching accommodates Nikon punctuation/case variants
(for example Z 7_2 and Z 7 2) and E5400/E8400/E8800 identifiers. Existing 40
sample-derived dual-illuminant calibrations are retained. Another 58 come from
numerical Adobe Standard DCP metadata; their filenames, hashes, and extracted
fields are recorded in [`camera_profiles_extra.json`](../src/camera_profiles_extra.json).
No rendering LUTs or Adobe runtime software are bundled. Additional entries have
no measured BaselineExposure override, so the DNG default of zero applies.

An unlisted camera can be inspected/decoded if its format is supported, but writing
a color DNG needs calibration. Add a verified entry or convert its originals to
calibrated Bayer DNG first. Native NEF/NRW WhiteLevel defaults to the decoded bit-depth
maximum; use `--white` when a lower saturation level is known. DNG uses its embedded
WhiteLevel.

Local checks decoded and exported calibrated crops from **87 of 89 NEF/NRW samples**,
with white balance present in all 87. The rejected samples were RGB small-size
and CMYG. Newly checked crops from nine NRW models and five older NEFs matched
fresh Adobe-converted sample values after accounting for border removal and the
P7700's 16-to-14-bit representation. This comparison covers raw sample values;
default crop, saturation level, and rendering can differ from Adobe.
Z 6 uncompressed/lossless and Z 7 lossless matched Adobe raw values
bit-for-bit. Nikon-curve decoding of the tested Z 6 lossy sample differed from
Adobe's linearization by at most 3 raw levels. White balance and crop matched.
Z 6/Z 8 bursts were also tested through stacking/SR. This is sample-based validation,
not a promise for every camera mode or firmware.

## Bayer DNG input

Supported files use classic TIFF in either byte order, even coded dimensions
between 2 and 32766, unsigned integer samples of 8–16 bits, a rectangular 2×2 RGB
Bayer pattern, and uncompressed or SOF3 lossless JPEG strips/tiles. JPEG supports
interleaved unsampled components, predictors 1–7, point transforms, and restarts.

The reader handles linearization tables, scalar/per-site black levels, active
area, integer default crop, white level, AsShotNeutral, orientation, and common
exposure/lens metadata. Embedded matrices, calibration signatures, forward matrices,
and supported profile tables are carried into reconstruction. A calibrated DNG
from another make does not require a Nikon calibration table entry.

Currently rejected:

- LinearRaw/RGB input, X-Trans/CMYG/non-Bayer CFA, floating-point, or odd coded dimensions.
- BigTIFF, JPEG XL, lossy JPEG, Deflate, separate JPEG scans, or subsampled JPEG.
- Row/column interleaving, non-square default scale, fractional crop, nonzero row/column black deltas.
- Required image opcodes, or white balance specified only through AsShotWhiteXY.

Some Adobe DNGs from Nikon Z lenses include required vignetting/distortion opcodes;
use their original NEFs. Optional opcodes may be skipped. Pixel opcodes, digests,
embedded originals, semantic masks, extra profiles, and spatial gain maps are not
copied into newly reconstructed images. Supported main calibration is preserved;
output is not an archival copy of every input tag.

Packed TIFF fixtures in both byte orders and lossless JPEG roundtrips are tested.
Ordinary Adobe Z 7 DNG input was verified against its original values. A generated
Bayer DNG can have odd output dimensions after an SR crop; raw converters can read
it, but this input reader requires even dimensions for quad-based alignment.

## Output

Outputs are 16-bit Bayer or three-channel camera-native LinearRaw DNG, using
uncompressed strips or 256×256 lossless JPEG tiles (JPEG strips for images narrower
than 256 pixels, for raw-reader compatibility). They carry reference calibration,
white balance, black/white levels, crop/orientation, source filename, and processing
description. LinearRaw samples remain in camera space, without white balance,
display color transformation, or tone curve applied.

Bayer output permits later demosaicing. LinearRaw has reconstructed three channels;
raw-converter features requiring a mosaic may be unavailable. Profile/denoising
availability depends on the raw converter and file type. Camera-rendering LUTs are
not shipped by this project.

This product includes DNG technology under license by Adobe.
