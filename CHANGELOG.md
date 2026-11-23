# Change history

## 0.2.0

- Shared NEV, NEF, and Bayer DNG input for conversion, stacking, SR, and folder inspection.
- Shared `--crop` for conversion, stacking, and SR, preserving Bayer/black-level phase.
- Z 6/Z 7 support and broader Nikon decoders/calibrations reused from nefcomp.
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
