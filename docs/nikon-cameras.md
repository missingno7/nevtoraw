# Nikon RGB calibration catalog

The bundled catalog contains **98 RGB camera calibrations**.

A listed calibration is not a claim that every capture mode was tested. Native input
must contain a supported RGB Bayer mosaic. CMYG and RGB small-raw modes are excluded.
See [format support and validation](raw-formats.md). Existing sample-verified entries
are retained; additional matrices come from local Adobe Standard DCP metadata.

Additional profiles do not invent a camera BaselineExposure value; its DNG default applies.

- Nikon 1 AW1
- Nikon 1 J1
- Nikon 1 J2
- Nikon 1 J3
- Nikon 1 J4
- Nikon 1 J5
- Nikon 1 S1
- Nikon 1 S2
- Nikon 1 V1
- Nikon 1 V2
- Nikon 1 V3
- Nikon Coolpix 5400
- Nikon Coolpix 8400
- Nikon Coolpix 8700
- Nikon Coolpix 8800
- Nikon Coolpix A
- Nikon Coolpix A1000
- Nikon Coolpix B700
- Nikon Coolpix P1000
- Nikon Coolpix P1100
- Nikon Coolpix P330
- Nikon Coolpix P340
- Nikon Coolpix P6000
- Nikon Coolpix P7000
- Nikon Coolpix P7100
- Nikon Coolpix P7700
- Nikon Coolpix P7800
- Nikon Coolpix P950
- Nikon D1
- Nikon D100
- Nikon D1H
- Nikon D1X
- Nikon D200
- Nikon D2H
- Nikon D2Hs
- Nikon D2X
- Nikon D2Xs
- Nikon D3
- Nikon D300
- Nikon D3000
- Nikon D300S
- Nikon D3100
- Nikon D3200
- Nikon D3300
- Nikon D3400
- Nikon D3500
- Nikon D3S
- Nikon D3X
- Nikon D4
- Nikon D40
- Nikon D40X
- Nikon D4S
- Nikon D5
- Nikon D50
- Nikon D500
- Nikon D5000
- Nikon D5100
- Nikon D5200
- Nikon D5300
- Nikon D5500
- Nikon D5600
- Nikon D6
- Nikon D60
- Nikon D600
- Nikon D610
- Nikon D70
- Nikon D700
- Nikon D7000
- Nikon D70s
- Nikon D7100
- Nikon D7200
- Nikon D750
- Nikon D7500
- Nikon D780
- Nikon D80
- Nikon D800
- Nikon D800E
- NIKON D810
- Nikon D810A
- Nikon D850
- Nikon D90
- Nikon Df
- NIKON Z 30
- Nikon Z 5
- Nikon Z 5 2
- Nikon Z 5 2C
- Nikon Z 50
- Nikon Z 50 2
- Nikon Z 6
- Nikon Z 6 2
- Nikon Z 6 3
- Nikon Z 7
- Nikon Z 7 2
- Nikon Z 8
- Nikon Z 9
- Nikon Z f
- Nikon Z fc
- Nikon ZR

Regenerate additional entries using `python tools/import_nikon_calibrations.py PATH_TO_ADOBE_STANDARD`.
Only numerical calibration metadata is extracted. Original profile files and LUTs are not shipped.
