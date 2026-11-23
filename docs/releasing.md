# Release validation

The supported release target is Windows x64 with MSYS2 MinGW-w64 and C++17.
The executable is statically linked. `src/version.h` is the version source for
`--version`, the DNG Software tag, and archive names. Update README, changelog,
and the CLI version assertion when changing it.

New CI builds use UCRT64, following the [MSYS2 environment recommendation](https://www.msys2.org/news/#2026-10-24-mingw64-deprecation-update).
The local release checks also run with the existing MINGW64 GCC toolchain.

## Local checks

From the UCRT64 shell in a fresh checkout (the existing MINGW64 toolchain also builds locally):

```sh
mingw32-make test
python tools/package_release.py
```

For a clean build, run `mingw32-make clean` first. The test target builds the CLI
and a native test executable, then runs generated fixtures and Python CLI checks.
No personal footage, network connection, Adobe Converter, or image-processing
Python packages are required for tests. Python's standard library is sufficient.
Test artifacts live under `out/`; do not commit them. `tests/cli.py` resets only
its own `out/cli-tests` directory on each run.

Coverage includes TIFF packing/byte order, lossless JPEG roundtrips and truncated
files, calibration/white balance/crop preservation, black offsets, full sample
range, frame selection, all-frame stacking defaults, scales 1–4, output types,
folder conversion/inspection, Unicode paths, invalid options, and source protection.

Where local camera files are available, additionally exercise:

```powershell
.\nevtoraw.exe info camera.NEF
.\nevtoraw.exe dng camera.NEF -o out\conversion
.\nevtoraw.exe stack burst --merge trimmed-mean -o out\stack
.\nevtoraw.exe sr burst --count 8 --crop 1000,1000,256,256 -o out\sr
.\nevtoraw.exe stack clip.NEV --count 8 --merge median -o out\video
```

Use Z 6/Z 7 lossless samples and Z 8 HE/HE★ for native decoder checks. Compare
decoded output sample arrays with prior accepted outputs or an independent raw
decoder, and inspect crops/color in a raw converter. File hashes can change solely
because the version or description metadata changed. Raw samples must be compared
separately. Check representative outputs from both compression modes.

## Build artifacts

`tools/package_release.py` verifies the binary's version and that it is no older
than its C++ sources. It creates:

- `out/release/nevtoraw-VERSION-windows-x64.zip`, including the executable,
  complete matching source, documentation, tests, and license notices.
- `out/release/nevtoraw-VERSION-source.zip`, the same source without a binary.
- `out/release/SHA256SUMS.txt`, checksums of both archives.

Each package has a `SOURCE-SHA256.txt` listing its source snapshot. Packaging uses
an explicit source-directory list, excludes footage/build output, and includes
uncommitted source changes so local review packages match the working tree. For
a public release, build/package from the final committed revision and verify
`git status --short` is empty. Never include `examples/`, `out/`, or personal footage.

Unpack the Windows archive into an empty directory, check `nevtoraw.exe --version`
and `--help`, and verify the archive checksums. Rebuilding the supplied source must
pass the same test target. The CI workflow performs Windows build, tests, packaging,
and artifact upload on pushes and pull requests; it does not publish GitHub releases.

Tagging, pushing, and publishing release assets are separate release operations.
