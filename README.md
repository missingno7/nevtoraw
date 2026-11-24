# nevtoraw

Export Nikon N-RAW (`.NEV`) frames as Bayer DNGs, stack handheld raw bursts, and
reconstruct higher-resolution images directly from their original CFA samples.
Version **0.2.0**, Windows and Linux x64, one standalone executable.

Download the binary archive and checksums from the
[v0.2.0 release](https://github.com/missingno7/nevtoraw/releases/tag/v0.2.0).
Windows uses `nevtoraw.exe`; Linux uses `nevtoraw` from the `.tar.gz` archive:

```sh
tar -xzf nevtoraw-0.2.0-linux-x64.tar.gz
./nevtoraw-0.2.0-linux-x64/nevtoraw --help
```

Both binary packages include matching source and license notices. Linux x64
requires glibc 2.35 or newer (Ubuntu 22.04 or newer, for example); it needs no
separately installed C++ runtime or raw decoder. Alpine/musl is not a release target.

## Quick start

```powershell
# Inspect a clip or see the numbered files in a still burst.
.\nevtoraw.exe info examples\4

# Align and merge the whole burst, retaining a Bayer mosaic for Camera Raw.
.\nevtoraw.exe stack examples\4 --merge trimmed-mean

# Reconstruct a 2x LinearRaw DNG from 16 evenly spaced frames.
.\nevtoraw.exe sr examples\DSC_8038.NEV --count 16

# Export original Bayer frames without stacking or super-resolution.
.\nevtoraw.exe dng examples\DSC_8038.NEV --frames 10-25 -o out\frames
```

`dng`, `stack`, and `sr` share one input loader. They accept a NEV clip, a NEF,
NRW or Bayer DNG still, or a folder containing one Nikon raw or DNG burst. Folder files are
sorted by filename, indexed from zero, without recursing into subfolders.
`info` shows the index-to-filename mapping before processing.

| Setting | `dng` | `stack` | `sr` |
| --- | --- | --- | --- |
| Candidates (`--frames`) | `0` | `all` | `all` |
| Frames used (`--count`) | Export every candidate | `all` | Up to `16`, spread evenly |
| Alignment | None | `homography` | Homography |
| Merge / reconstruction | Original samples | Robust `mean` | Gaussian local fit, sigma `0.30` |
| Output | Bayer DNG per frame | One Bayer DNG | One `2x` LinearRaw DNG |
| Compression | Lossless JPEG | Lossless JPEG | Lossless JPEG |

The middle selected frame is the default reference for `stack` and `sr`.
For mostly static scenes, `--merge trimmed-mean` averages the middle 60% of valid
samples by default. Use `median` for stronger rejection, or the default robust
`mean` to preserve moving subjects at their reference-frame position.

## Choose frames and resolution

`--frames` defines candidates. `--count N` takes N frames evenly spaced through
those candidates, including the reference. `--count all` uses all of them.
Ranges include both endpoints. These options work identically in `stack` and `sr`.

```powershell
# Sixteen consecutive frames.
.\nevtoraw.exe sr clip.NEV --frames 10-25 --count all

# Ten frames spread across frames 10 through 50.
.\nevtoraw.exe sr clip.NEV --frames 10-50 --count 10

# Sixteen frames, ten frames apart.
.\nevtoraw.exe stack clip.NEV --frames 10-160:10 --merge median

# Native-size linear raw, or a larger reconstruction with a narrower kernel.
.\nevtoraw.exe sr clip.NEV --count 256 --scale 1 --kernel-sigma 0.20
.\nevtoraw.exe sr clip.NEV --count 64 --scale 3 --kernel-sigma 0.20
```

SR supports `--scale 1|2|3|4`, default `2`. Scale is magnification on each axis,
so 2x creates four times as many output pixels. `--kernel-sigma` is always measured
in **input sensor pixels**, independent of scale. The `0.30` default is a useful
starting point; `0.42` smooths more, while `0.20` or `0.10` needs more sharp frames
and varied subpixel positions. Higher scale alone cannot create missing detail.
Sparse regions blend back toward interpolation from the reference.

`--crop X,Y,W,H` is shared by `dng`, `stack`, and `sr`. Coordinates refer to the
source's coded sensor grid (the reference for a merge), before orientation and SR
scaling. Conversion extracts the original samples; stacking aligns/merges the full
frame and crops its result; SR reconstructs the requested region. For example:

```powershell
.\nevtoraw.exe stack examples\4 --merge trimmed-mean --crop 1000,1000,256,256
.\nevtoraw.exe dng photo.NEF --crop 1000,1000,256,256
```

Use a small crop for initial SR experiments. `--bayer` requests a mosaic
instead of the default camera-native RGB LinearRaw DNG. Both retain calibration
and white balance for subsequent processing.

## Inputs and outputs

Native NEF/NRW decoding comes from [nefcomp](https://github.com/missingno7/nefcomp).
The bundled [98-model Nikon catalog](docs/nikon-cameras.md) covers RGB Bayer models
across Z, DSLR, Nikon 1, and raw-capable Coolpix families. Run
`.\nevtoraw.exe cameras` to list calibrations. CMYG sensors and RGB small-raw modes
are excluded; calibration coverage does not guarantee every capture mode.
Other cameras can use calibrated Bayer DNG input. See the exact
[camera and DNG limits](docs/raw-formats.md); DNG support is a defined subset,
including uncompressed and lossless JPEG integer Bayer files.

Single-file results go beside the source by default. Folder results go into
`SOURCE\nevtoraw`, which is excluded from subsequent scans. `-o DIR` selects another
output directory. DNG conversion adds `_converted` to the filename. Generated
outputs can be replaced by a rerun; source files are protected from replacement.
Failed writes leave existing outputs intact.

Execution settings are automatic across conversion, stacking, and SR. A shared
worker pool overlaps decoding with processing; the default RAM budget is 70% of
currently available memory. Large frame/output caches spill to temporary storage.
Use `--memory-limit 8G` to constrain RAM, `--threads 8` to limit workers, or
`--temp-dir D:\scratch` to select scratch storage. The program reports its budget
and stage timings. See [execution and memory](docs/runtime.md) for working-memory
minimums and cache behavior.

Stacking preserves a Bayer mosaic and averages in linear raw space. SR reconstructs
camera-native channels from aligned CFA samples. It applies no display tone curve
or white balance to the sample values. Handheld movement can provide useful
subpixel coverage, but parallax, rolling shutter, subject motion, and blur can
limit a global homography. SR has no motion-rejection merge mode; prefer static scenes.

## Build and test

Use the MSYS2 MinGW-w64 C++17 toolchain on Windows x64. From its UCRT64 shell:

```sh
pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-make mingw-w64-ucrt-x86_64-python
mingw32-make
mingw32-make test
```

On Linux x64, use GCC, Make, and Python 3 (on Ubuntu: `sudo apt install build-essential python3`):

```sh
make test
./nevtoraw --version
python3 tools/package_release.py
```

Linux links its C++ runtime statically and uses the OS C runtime. Override
`LDFLAGS` for other local linking choices. Release CI builds and tests both platforms.

No Adobe Converter, `nefcomp`, LibRaw installation, or additional runtime DLLs are
needed to use the executable. Python is used only by CLI tests and release packaging.
Tests generate their own DNG fixtures and do not need personal footage.

`--help` works globally and after any command. `--version` prints the build version.
The [CLI reference](docs/cli.md) covers every option, memory use, filenames, and
failure behavior. See [release validation](docs/releasing.md),
[development measurements](docs/development-notes.md), and [change history](CHANGELOG.md).

## License

GPL-2.0-or-later, with bundled decoder components under LGPL-2.1/CDDL.
See [COPYING](COPYING) and [third-party notices](THIRD_PARTY.md).
This product includes DNG technology under license by Adobe.
