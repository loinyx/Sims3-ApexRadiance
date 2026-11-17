# Multiplicative apply error: tables

Written by `run.py --write`. Levels are 8-bit levels of the finished pixel; positive means the plain apply is too bright.

### 1. Floor pixels: error of `colour *= 1 + s` in 8-bit levels (grey pixel, grey term)

| pixel mean (0-255) | error per unit s, small s | s = -1 | s = -0.5 | s = -0.25 | s = -0.1 | s = -0.05 | s = +0.05 | s = +0.1 | s = +0.25 | s = +0.5 | s = +1 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 0.1 (26) | 0.7% | +0.0 | -0.0 | -0.0 | -0.0 | -0.0 | +0.0 | +0.0 | +0.1 | +0.2 | +0.5 |
| 0.2 (51) | 2.7% | +0.0 | -0.3 | -0.2 | -0.1 | -0.1 | +0.1 | +0.2 | +0.5 | +1.3 | +3.9 |
| 0.3 (76) | 6.1% | +0.0 | -0.9 | -0.8 | -0.4 | -0.2 | +0.3 | +0.5 | +1.6 | +4.2 | +12.6 |
| 0.4 (102) | 11.0% | +0.0 | -2.2 | -1.9 | -1.0 | -0.5 | +0.6 | +1.3 | +3.8 | +9.7 | +28.1 |
| 0.5 (128) | 17.6% | +0.0 | -4.6 | -3.9 | -2.0 | -1.1 | +1.2 | +2.5 | +7.4 | +18.6 | +51.0 |
| 0.6 (153) | 26.1% | +0.0 | -8.5 | -7.0 | -3.5 | -1.9 | +2.1 | +4.5 | +12.9 | +31.2 | +30.0 |
| 0.7 (178) | 36.8% | +0.0 | -14.9 | -12.0 | -5.9 | -3.1 | +3.5 | +7.2 | +20.5 | +35.2 | +15.4 |
| 0.8 (204) | 50.6% | +0.0 | -25.5 | -19.7 | -9.4 | -4.9 | +5.4 | +11.2 | +30.7 | +18.2 | +6.2 |
| 0.9 (230) | 68.9% | +0.0 | -45.1 | -32.4 | -14.8 | -7.7 | +8.1 | +16.7 | +12.5 | +6.1 | +1.4 |

### 2. Largest floor error over all grey pixel values up to 229 (map 1 x albedo 1), and the pixel mean where it falls

| s | largest error (levels) | at pixel mean | brightest pixel mean with at most 1 / 2 / 4 levels |
|---|---|---|---|
| -1 | 0.0 | 228 | 228 / 228 / 228 |
| -0.5 | 44.0 | 228 | 79 / 98 / 122 |
| -0.25 | 31.7 | 228 | 82 / 104 / 128 |
| -0.1 | 14.5 | 228 | 102 / 128 / 159 |
| -0.05 | 7.5 | 228 | 126 / 156 / 192 |
| +0.05 | 8.0 | 228 | 120 / 150 / 186 |
| +0.1 | 16.4 | 228 | 94 / 118 / 148 |
| +0.25 | 30.7 | 204 | 65 / 82 / 104 |
| +0.5 | 41.9 | 170 | 47 / 60 / 75 |
| +1 | 51.0 | 128 | 32 / 40 / 51 |

### 3. Coloured cases (floor pixel; error of the worst channel, levels)

| pixel (0-255) | term s (r, g, b) | plain apply | truth | error |
|---|---|---|---|---|
| 153, 153, 153 | +0.3, +0, +0 | 199, 153, 153 | 194, 149, 149 | +5.3 |
| 153, 153, 153 | +0.3, +0.15, +0 | 199, 176, 153 | 191, 169, 147 | +7.9 |
| 204, 76, 51 | +0.25, +0.25, +0.25 | 255, 96, 64 | 246, 92, 62 | +8.9 |
| 204, 76, 51 | -0.5, -0.5, -0.5 | 102, 38, 26 | 107, 40, 27 | -5.3 |
| 51, 76, 204 | +0.5, +0.1, +0 | 77, 84, 204 | 75, 83, 201 | +2.7 |
| 178, 178, 153 | -0.25, -0.25, -0.25 | 134, 134, 115 | 144, 144, 124 | -10.6 |

### 4. Maps the game still solves: the solve curve alone (walls, objects) and before the floor curve

Relative error of the plain apply, percent of the true pixel. Walls: by the map mean. Floors: by the pixel mean, albedo 0.65 and 1.

| surface | mean | s = -0.5 | s = -0.1 | s = +0.1 | s = +0.5 |
|---|---|---|---|---|---|
| wall, map mean | 0.2 | -1.0% | -0.3% | +0.3% | +1.7% |
| wall, map mean | 0.4 | -4.2% | -1.1% | +1.2% | +6.8% |
| wall, map mean | 0.6 | -10.0% | -2.5% | +2.7% | +15.7% |
| wall, map mean | 0.8 | -20.0% | -4.9% | +5.2% | +29.2% |
| floor, albedo 0.65, pixel mean | 0.2 | -2.2% | -0.5% | +0.6% | +3.5% |
| floor, albedo 0.65, pixel mean | 0.4 | -9.1% | -2.2% | +2.4% | +14.0% |
| floor, albedo 0.65, pixel mean | 0.6 | -23.2% | -5.5% | +5.8% | +31.4% |
| floor, albedo 0.65, pixel mean | 0.8 | - | - | - | - |
| floor, albedo 1, pixel mean | 0.2 | -1.5% | -0.4% | +0.4% | +2.4% |
| floor, albedo 1, pixel mean | 0.4 | -6.2% | -1.5% | +1.7% | +9.8% |
| floor, albedo 1, pixel mean | 0.6 | -15.0% | -3.6% | +3.9% | +21.8% |
| floor, albedo 1, pixel mean | 0.8 | -31.1% | -7.1% | +7.4% | +15.7% |
