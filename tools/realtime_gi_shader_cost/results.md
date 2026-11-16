# Real-time GI shader cost: results

Written by `run.py --write`.

Compiler: fxc 10.1, `/T ps_3_0 /O3`. Limits: 512 slots, 32 temporaries, 16 samplers, 224 constants, 10 inputs, 65535 instructions run.
Hosts for the verdict of attached snippets: 59, 170, 158, 343 slots.

| Snippet | Slots | Added to host | Run | Texture reads | Temporaries | Constants | Samplers | Inputs | Verdict |
|---|---|---|---|---|---|---|---|---|---|
| host stand-in (albedo x ambient) | 3 | +0 | 3 | 1 | 1 | 1 | 1 | 1 | fits every host |
| 1 lamp, constants | 21 | +18 | 21 | 1 | 3 | 4 | 1 | 3 | fits every host |
| 1 lamp + cone | 25 | +22 | 25 | 1 | 3 | 5 | 1 | 3 | fits every host |
| 1 lamp + shadow, 1 compare | 48 | +45 | 48 | 3 | 6 | 7 | 2 | 3 | fits every host |
| 1 lamp + shadow, 4 taps averaged | 76 | +73 | 76 | 9 | 6 | 8 | 2 | 3 | fits every host |
| 1 lamp + shadow, 4 taps bilinear | 79 | +76 | 79 | 9 | 7 | 8 | 2 | 3 | fits every host |
| 1 lamp + cone + shadow, 4 taps bilinear | 83 | +80 | 83 | 9 | 7 | 9 | 2 | 3 | fits every host |
| index cell read, 4 lamps x 2 texldl, no light | 48 | +45 | 48 | 19 | 5 | 5 | 2 | 2 | fits every host |
| index cells, 4 lamps | 95 | +92 | 95 | 19 | 6 | 5 | 2 | 3 | fits every host |
| index cells, 4 lamps + 1 compare | 200 | +197 | 200 | 27 | 11 | 8 | 2 | 3 | fits hosts up to 170 slots |
| index cells, 4 lamps + 4 taps bilinear | 319 | +316 | 319 | 51 | 13 | 9 | 2 | 3 | fits hosts up to 170 slots |
| index cells, 4 lamps + cone (3 texldl) + 4 taps bilinear | 348 | +345 | 348 | 59 | 15 | 9 | 2 | 3 | fits hosts up to 158 slots |
| index cells, 8 lamps + 1 compare | 381 | +378 | 381 | 53 | 12 | 8 | 2 | 3 | fits hosts up to 59 slots |
| index cells, 8 lamps + 4 taps bilinear | 616 | +613 | 616 | 101 | 13 | 9 | 2 | 3 | too large for any host |
| index cells, 8 lamps + cone + 4 taps bilinear | 672 | +669 | 672 | 117 | 15 | 9 | 2 | 3 | too large for any host |
| index cells, 8 lamps + cone + 4 taps bilinear, loop | 113 | +110 | 708 | 17 | 16 | 10 | 2 | 3 | fits every host |
| index cells, 16 lamps + cone + 4 taps bilinear, loop | 113 | +110 | 1388 | 17 | 16 | 10 | 2 | 3 | fits every host |
| compose, 16 layers, plain bilinear | 68 | - | 68 | 20 | 5 | 19 | 3 | 1 | fits |
| compose, 16 layers, wall-aware | 408 | - | 408 | 140 | 11 | 20 | 3 | 1 | fits |
| compose, 8 layers, wall-aware | 235 | - | 235 | 76 | 11 | 12 | 3 | 1 | fits |
| plan sphere trace, 8 steps, unrolled | 105 | - | 105 | 16 | 5 | 3 | 1 | 1 | fits |
| plan sphere trace, 16 steps, unrolled | 201 | - | 201 | 32 | 5 | 3 | 1 | 1 | fits |
| plan sphere trace, 16 steps, loop | 31 | - | 256 | 2 | 4 | 4 | 1 | 1 | fits |
| gather, 16 virtual lights, no visibility | 321 | - | 321 | 98 | 12 | 7 | 3 | 1 | fits |
| gather, 16 virtual lights, room table | 419 | - | 419 | 130 | 14 | 7 | 3 | 1 | fits |
| gather, 16 x trace 8, unrolled | 2099 | - | 2099 | 386 | 16 | 9 | 3 | 1 | over 512 slots |
| gather, 4 x trace 8, unrolled | 535 | - | 535 | 98 | 13 | 6 | 3 | 1 | over 512 slots |
| gather, 16 x trace 8, loops | 74 | - | 2609 | 12 | 11 | 6 | 3 | 1 | fits |
| gather, 16 x trace 16, loops | 74 | - | 4529 | 12 | 11 | 6 | 3 | 1 | fits |
| gather, 16 x trace 8, loops, 4 directional targets | 88 | - | 2698 | 12 | 13 | 6 | 3 | 1 | fits |
| gather, 64 x trace 16, loops | 74 | - | 18065 | 12 | 11 | 6 | 3 | 1 | fits |
