# PAT9125EL reports less travel than actually occurred

Minimum reproducible case. Two variants, one per part number — pick the one that
matches your chip. Single file each, no libraries, builds in the Arduino IDE or
PlatformIO.

Both sketches drive **two sensors on one shared bus**, matching the rig where this
was observed. That matters: *both* sensors show the same shortfall, which is why a
single faulty part is not the explanation.

| | Part | Interface | Folder |
|---|---|---|---|
| SPI | `PAT9125EL-TKMT` | 3-wire SPI | [`spi/`](spi/) |
| I2C | `PAT9125EL-TKIT` | I2C | [`i2c/`](i2c/) |

The two part numbers are **not interchangeable** (AN01 §1.1), so only one will
respond. If neither prints `0x31`, that is a wiring/power fault rather than the
behaviour reported here — see the troubleshooting notes in each sketch header.

## The question

A PAT9125EL tracking a **~2 mm diameter rod** consistently reports **less**
displacement than the rod actually travels, and the shortfall **grows with speed**:

| Speed | Reported ÷ actual | Spread |
|---|---|---|
| ~3.7 mm/s | 0.912, 0.918, 0.918 | ±0.3% |
| ~12.4 mm/s | 0.875, 0.880 | ±0.3% |

Roughly **8% short at 3.7 mm/s, 12% short at 12.4 mm/s** — highly repeatable within
a speed, across many runs and both of two sensors, which is why we read it as
systematic rather than noise.

**Is this expected for this part on a cylindrical target, and if so what standoff
and aperture geometry is required to avoid it?**

## Configuration

| | |
|---|---|
| Resolution | `RES_X = RES_Y = 0xFF` → 1275 cpi → **50.2 counts/mm** nominal |
| Data format | `ORIENTATION = 0x04` (12-bit) |
| Supply | VDD 3.3 V, VLD 3.3 V |
| Target | ~2 mm rod, sliding axially past both sensors |
| Sensors | 2, on one shared bus (SPI: shared SCLK/SDIO + separate NCS; I2C: shared SCL/SDA + different `ID_SEL`) |

## Build

```
cd spi        # or: cd i2c
pio run -t upload
```
or open `spi/pat9125_mre_spi/pat9125_mre_spi.ino` in the Arduino IDE. No libraries.

## Reproduce

1. Flash and open the serial monitor at 115200. It must report product ID `0x31`.
2. Send `z` to zero the counters.
3. Move the target a **ruler- or caliper-measured** distance at a steady, known speed.
4. Compare the printed `x_mm` against the true distance.
5. Repeat at a faster speed and compare the two ratios.

Output is CSV, both sensors per row:

```
ms,s1_dx,s1_dy,s1_x,s1_y,s1_x_mm,s1_shutter,s1_frame_avg,s2_dx,...,s2_frame_avg
```

**`s1_x` / `s2_x` (raw counts) are the primary data** — the `_mm` columns apply the
nominal 50.2 counts/mm, and that scale factor is part of what is in question.
Comparing the two sensors against each other is itself informative: they see the same
rod through separate optics, so agreement between them points away from one bad part.

### Optical diagnostics

Each row also logs two registers that bear directly on the suspected cause:

| Reg | Name | Reads |
|---|---|---|
| `0x14` | **Shutter** | exposure-time index — the chip lengthens it when the image is dim |
| `0x17` | **Frame_Avg** | mean image brightness |

These matter because they turn an optical problem into a measurable one. A dim image
makes the chip lengthen its exposure; a longer exposure means more motion blur per
frame; more blur at higher speed means degraded correlation and lost counts — which is
precisely the speed-dependent shortfall reported above.

So the diagnostic question is: **does `Shutter` climb, or `Frame_Avg` sit low, and does
either change between the slow and fast runs?** If so, the cause is illumination and
aperture geometry rather than mechanics.

## Running the optical test

Press **`t`** to step through three phases, pressing `t` again to end each:

| Phase | Do this |
|---|---|
| 1 — REST | hold everything still |
| 2 — SLOW | move the target slowly |
| 3 — FAST | move the target fast |

It then prints the measured table and an interpretation:

```
=== OPTICAL DIAGNOSTIC (sensor 1) ===
  phase   speed           shutter min/mean/max   frame_avg min/mean/max
  REST    0.00 mm/s       12/12.0/13             98/98.4/99
  SLOW    3.70 mm/s       18/19.2/21             71/72.0/74
  FAST    12.40 mm/s      31/33.8/36             54/55.1/57

  shutter   rest->fast: +181.7%
  frame_avg rest->fast: -44.0%
```

### Reading the result

| Observation | Means | Points at |
|---|---|---|
| Shutter climbs >25% rest→fast | chip is short of light and lengthening exposure; longer exposure blurs more per frame, and blur grows with speed | **aperture / illumination** — consistent with the speed-dependent loss |
| `Frame_Avg` very low (<25) | little light reaching the array | aperture size, standoff, or a shadowed VCSEL |
| `Frame_Avg` near saturation (>230) | ambient light swamping the VCSEL | shielding — **and note enlarging the aperture makes this worse** |
| Both stable and mid-range | illumination is fine | **not** optical — look to standoff variation, scale calibration, or mechanics |

The thresholds are heuristics; the table of numbers is the actual evidence. Take the
REST baseline *before* modifying any hardware, so there is something to compare against.

## Already ruled out

- **Polling rate.** The chip accumulates between reads and a read clears the
  registers. We poll at 739–1360 Hz; at 30 mm/s the 12-bit delta register would take
  1.29 s to saturate — a ~1000× margin. Counts are not lost between reads.
- **Bus errors.** 100% read success over these runs, product ID stable at `0x31`.
- **In-plane (yaw) misalignment.** Fitting the lateral channel against the travel
  axis gives 0.6–1.0°, worth a 0.01% cosine error. ~24° would be needed for 9%.
- **Register settings.** `RES_X`/`RES_Y` are read back after init, so a silent write
  failure cannot masquerade as a scale error.

## What we suspect

1. **Aperture too small.** Datasheet §4.5.1 requires a detection area larger than
   **3.2 × 2.6 mm** (asymmetric — 1.45 mm / 1.75 mm about centre, because it includes
   the VCSEL) and warns that undersizing it "could cause the decrease of tracking
   speed and stability of the chip". Ours is around 2 mm. Enough to explain an 8–12%
   deficit and the speed dependence?
2. **Standoff variation.** The rod has radial clearance; we measure 0.13–0.2 mm of
   lateral wander during travel, so standoff likely varies similarly. How sensitive is
   effective counts/mm to standoff on this part?
3. **Curved target.** The datasheet separates "flat SUS" (DOF 1–30 mm) from "SUS
   shaft" (DOF 1–10 mm), quoting shaft speed on a 1.0 mm diameter. For a ~2 mm rod,
   what standoff and aperture do you recommend, and what scale accuracy is achievable?
