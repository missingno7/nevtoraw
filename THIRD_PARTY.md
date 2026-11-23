# Third-party notices

The application is GPL-2.0-or-later; see [COPYING](COPYING).

- `third_party/nikon-he-public`: public HE decoder from LibRaw PR #826 (Dmitri Sotnikov),
  LGPL-2.1 or CDDL. Bundled LICENSE.LGPL and LICENSE.CDDL contain the terms; source
  receipts record provenance. Local subband fixes are documented in development notes.
- `src/he_decode.cpp`: GPL-2.0-or-later bridge adapted from
  [nefcomp](https://github.com/missingno7/nefcomp) and
  [WonderLantern z8-qemu](https://github.com/WonderLantern/z8-qemu).
- Nikon Huffman decoding and packing routines: adapted from nefcomp,
  GPL-2.0-or-later, with local input validation.
- Camera matrices: numerical calibration metadata extracted from Adobe-converted
  local Nikon samples and installed Adobe Standard DCPs. Additional matrix fields,
  source filenames, and hashes are in `src/camera_profiles_extra.json`; the importer
  exports only camera identity, color matrices, and illuminants. DCP files, rendering
  LUTs, and Adobe software are not bundled.
- Nikon metadata layout facts were cross-checked against
  [LibRaw's Nikon metadata reader](https://github.com/LibRaw/LibRaw/blob/master/src/metadata/nikon.cpp)
  and [ExifTool's Nikon tag definitions](https://github.com/exiftool/exiftool/blob/master/lib/Image/ExifTool/Nikon.pm).

This product includes DNG technology under license by Adobe. Nikon, Adobe, and
intoPIX are trademarks of their respective owners. This is an independent
interoperability project; no endorsement is implied.

Windows binaries statically link MinGW-w64/GCC runtime components. The GCC Runtime
Library Exception applies to eligible runtime components. Source packages contain
the application, decoder sources/licenses, tests, and build instructions needed to
rebuild the executable.
