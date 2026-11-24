# Release validation

Release targets are Windows x64 with MSYS2 MinGW-w64 and Linux x64 with GCC/C++17.
Windows links its non-system dependencies statically. Linux CI uses Ubuntu 22.04,
links GCC/C++ runtimes statically, and uses the OS glibc 2.35 or newer.
`src/version.h` is the version source for
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

On Linux x64, use `make test` followed by `python3 tools/package_release.py`.
The Makefile selects native executable names, POSIX 64-bit file seeking, and
pthread flags automatically. `TARGET`, `TEST_TARGET`, and `PYTHON` can be overridden.

For a clean build, run `mingw32-make clean` first. The test target builds the CLI
and a native test executable, then runs generated fixtures and Python CLI checks.
No personal footage, network connection, Adobe Converter, or image-processing
Python packages are required for tests. Python's standard library is sufficient.
Test artifacts live under `out/`; do not commit them. `tests/cli.py` resets only
its own `out/cli-tests` directory on each run.

When `dcraw` is available, the CLI suite also compares small lossless-JPEG crop
samples with that independent decoder. The core suite does not require it.

Coverage includes TIFF packing/byte order, lossless JPEG roundtrips and truncated
files, calibration/white balance/crop preservation, black offsets, full sample
range, frame selection, all-frame stacking defaults, scales 1–4, output types,
folder conversion/inspection, Unicode paths, invalid options, and source protection.
Nikon fixtures cover native NEF/NRW folder input, old/NRW white-balance blocks,
Coolpix black-level scales, model aliases/calibration selection, the DCP importer,
and D1X sensor aspect. Tests use synthetic metadata, not bundled camera originals.
Execution checks compare resident and forced disk-backed median/trimmed stacks
and SR outputs, verify worker/prefetch exceptions, nested pool work, scratch-file
cleanup, streamed write failure atomicity, and shared memory/thread CLI controls.

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
For broader Nikon changes, include DSLR/Nikon 1/Coolpix NEFs and NRW files. A
catalog entry verifies calibration availability; it does not substitute for a
capture-mode test. The current local corpus results are in [raw formats](raw-formats.md).

## Build artifacts

`tools/package_release.py` verifies the binary's version and that it is no older
than its C++ sources. It creates:

- `out/release/nevtoraw-VERSION-windows-x64.zip`, including the executable,
  complete matching source, documentation, tests, and license notices.
- `out/release/nevtoraw-VERSION-linux-x64.tar.gz` when packaging on Linux, with
  the same source and notices plus an executable with mode 0755 and its C++ runtime included.
- `out/release/nevtoraw-VERSION-source.zip`, the same source without a binary.
- `out/release/SHA256SUMS.txt`, checksums of that build's archives.

The packager defaults to the host platform and supports `--platform`, `--binary`,
and `--output` overrides. It verifies the executable architecture and version.

Each package has a `SOURCE-SHA256.txt` listing its source snapshot. Packaging uses
an explicit source-directory list, excludes footage/build output, and includes
uncommitted source changes so local review packages match the working tree. For
a public release, build/package from the final committed revision and verify
`git status --short` is empty. Never include `examples/`, `out/`, or personal footage.

Unpack each binary archive into an empty directory, check `--version` and `--help`,
and verify archive checksums. For Linux, verify the extracted executable can run
and `readelf -d nevtoraw` has no libstdc++ or libgcc_s dependency. Rebuilding the supplied source must
pass the same test target. The CI workflow performs Windows build, tests, packaging,
and artifact upload on pushes and pull requests; it does not publish GitHub releases.

Tagging, pushing, and publishing release assets are separate release operations.
For a release, wait for both platform jobs at the intended commit, collect their
binary archives and one source archive, and verify their SOURCE-SHA256 manifests
match. Generate a combined SHA256SUMS.txt for the three archives. Create the
version tag at that exact commit and publish all four assets together.
