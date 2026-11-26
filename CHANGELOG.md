# Change history

## Unreleased

- `--dark PATH` measures per-colour black levels from dark frames, and `--black`
  sets them directly, for `dng`, `stack`, and `sr`. This removes the colour cast in
  stacked shadows caused by camera black levels a fraction of a DN too high.
- `--black auto` for `stack` and `sr` normalizes the merged result so the darkest 64×64 px
  areas average exactly black, per CFA site, without dark frames. `--black pixel`
  uses the single darkest merged sample instead, for comparison.
- Stack alignment limits Catmull-Rom interpolation to the nearest samples' range.
  Bright lights no longer leave dark rims hundreds of DN below black, which raw
  converters clipped to black dots around street lights in night bursts.

## 0.2.0 — 2026-11-24

- Shared NEV, NEF, NRW, and Bayer DNG input for conversion, stacking, SR, and folder inspection.
- Windows x64 and Linux x64 binaries with the C++ runtime included, both built
  and tested in CI and packaged with matching source and checksums.
- Shared `--crop` for conversion, stacking, and SR, preserving Bayer/black-level phase.
- Lossless JPEG strips for small crops, avoiding padded-row decoding errors in raw converters.
- Z 6/Z 7 support and broader Nikon decoders/calibrations reused from nefcomp.
- 98 Nikon RGB calibrations, normalized model aliases, and a `cameras` catalog command.
- Older Nikon/Coolpix white balance, NRW CFA and black-level metadata, and D1X sensor aspect.
- Shared persistent workers, bounded decode prefetch, automatic RAM budgeting, and
  disk-backed frame/output caches across conversion, stacking, and SR.
- Common `--memory-limit`, `--threads`, and `--temp-dir`; stage work timings and
  streamed atomic DNG output without a second complete file buffer.
- Homography by default; all frames by default for stacking.
- Robust mean, median, and trimmed-mean Bayer stacking with per-CFA black correction.
- 1x–4x raw SR; 2x LinearRaw and sigma 0.30 by default.
- Singular `--count N|all` and consistent selection within `--frames` candidates.
- Calibrated Bayer DNG input with integer uncompressed/lossless JPEG support.
- Shared version reporting, command help, atomic outputs, and source collision checks.
- Synthetic raw/selection/CLI regression tests, Windows build CI, and release packaging.

Experimental `--counts` is replaced by `--count`. Run separately for each comparison
count. Folder outputs default to `SOURCE/nevtoraw`, excluding generated files from
the next run. Stacking defaults to the entire burst; use `--frames`/`--count` to limit it.
