# PROBE-PREDICT (Wave 1b) Empirical Report

Candidate evaluation of channel predictors on image (BMP/TIFF/PPM/TGA/Kodak) and audio (WAV/RF64/AIFF/CAF) fixtures.

| File | Size | Format | Optimal Emittable DELTA | Baseline H | Residual H | H Gain | LZ Match Gain | Upper Bound (Paeth H) |
|:---|:---:|:---:|:---:|:---:|:---:|:---:|:---:|:---:|
| `.gitattributes` | 127 B | audio/raw | ch=1 | 4.607 | 5.687 | -1.079 | -0.833% | N/A |
| `audio_5.1_16bit.wav` | 48044 B | audio/raw | ch=12 | 7.961 | 6.899 | +1.062 | +43.985% | N/A |
| `audio_5.1_24bit.wav` | 72044 B | audio/raw | ch=18 | 7.982 | 7.434 | +0.549 | +35.990% | N/A |
| `audio_5.1_32bit.wav` | 96044 B | audio/raw | ch=24 | 7.990 | 7.648 | +0.342 | +30.254% | N/A |
| `audio_7.1_16bit.wav` | 64044 B | audio/raw | ch=16 | 7.950 | 7.004 | +0.946 | +52.857% | N/A |
| `audio_7.1_24bit.wav` | 96044 B | audio/raw | ch=24 | 7.975 | 7.491 | +0.483 | +38.363% | N/A |
| `audio_7.1_32bit.wav` | 128044 B | audio/raw | ch=32 | 7.982 | 7.689 | +0.293 | +29.354% | N/A |
| `audio_junk_prefixed.wav` | 16180 B | audio/raw | ch=4 | 7.943 | 6.721 | +1.222 | +42.707% | N/A |
| `audio_rf64_5.1_24bit.rf64` | 72080 B | audio/raw | ch=18 | 7.982 | 7.433 | +0.549 | +35.969% | N/A |
| `audio_sample_5.1_16bit.aiff` | 48054 B | audio/raw | ch=12 | 7.960 | 6.898 | +1.062 | +43.963% | N/A |
| `audio_sample_5.1_24bit.caf` | 72068 B | audio/raw | ch=18 | 7.982 | 7.433 | +0.550 | +35.974% | N/A |
| `bmp_w1919_16bpp.bmp` | 122934 B | BMP | ch=2 | 7.769 | 1.175 | +6.595 | +54.645% | 1.193 |
| `bmp_w1919_24bpp.bmp` | 184374 B | BMP | ch=3 | 7.787 | 3.529 | +4.257 | +25.456% | 3.750 |
| `bmp_w1919_32bpp.bmp` | 245686 B | BMP | ch=4 | 6.650 | 3.140 | +3.510 | +41.087% | 3.336 |
| `bmp_w1919_8bpp.bmp` | 62518 B | BMP | ch=1 | 7.757 | 3.664 | +4.093 | +0.114% | 3.625 |
| `bmp_w1920_16bpp.bmp` | 122934 B | BMP | ch=2 | 7.770 | 1.167 | +6.602 | +54.802% | 1.191 |
| `bmp_w1920_24bpp.bmp` | 184374 B | BMP | ch=3 | 7.786 | 3.522 | +4.263 | +25.482% | 3.752 |
| `bmp_w1920_32bpp.bmp` | 245814 B | BMP | ch=4 | 6.649 | 3.140 | +3.509 | +41.155% | 3.336 |
| `bmp_w1920_8bpp.bmp` | 62518 B | BMP | ch=1 | 7.756 | 3.665 | +4.092 | +0.091% | 3.630 |
| `bmp_w1921_16bpp.bmp` | 123062 B | BMP | ch=2 | 7.769 | 1.175 | +6.595 | +54.589% | 1.193 |
| `bmp_w1921_24bpp.bmp` | 184502 B | BMP | ch=3 | 7.786 | 3.523 | +4.262 | +24.450% | 3.750 |
| `bmp_w1921_32bpp.bmp` | 245942 B | BMP | ch=4 | 6.650 | 3.138 | +3.512 | +40.421% | 3.336 |
| `bmp_w1921_8bpp.bmp` | 62646 B | BMP | ch=1 | 7.756 | 3.670 | +4.086 | +0.089% | 3.634 |
| `photo_kodim01.bmp` | 1179702 B | BMP | ch=3 | 7.327 | 5.883 | +1.444 | -5.422% | 5.555 |
| `photo_kodim02.bmp` | 1179702 B | BMP | ch=3 | 6.802 | 4.641 | +2.161 | -23.046% | 4.366 |
| `photo_kodim03.bmp` | 1179702 B | BMP | ch=3 | 7.455 | 4.049 | +3.406 | -16.018% | 3.959 |
| `photo_kodim04.bmp` | 1179702 B | BMP | ch=3 | 7.492 | 4.881 | +2.612 | -7.802% | 4.568 |
| `sample.pgm` | 65551 B | PGM | ch=1 | 7.738 | 3.567 | +4.172 | +0.104% | 3.570 |
| `sample.ppm` | 196623 B | PPM | ch=3 | 7.765 | 3.576 | +4.189 | +0.073% | 3.572 |
| `sample.tga` | 196626 B | TGA | ch=3 | 7.765 | 3.584 | +4.181 | +0.059% | 3.577 |
| `sample_be.tif` | 49280 B | TIFF | ch=3 | 7.775 | 3.648 | +4.127 | +0.028% | 3.607 |
| `sample_le.tif` | 49280 B | TIFF | ch=3 | 7.775 | 3.648 | +4.127 | +0.028% | 3.607 |
| `text_utf16_le.txt` | 16200 B | audio/raw | ch=18 | 3.303 | 3.581 | -0.277 | -0.284% | N/A |
