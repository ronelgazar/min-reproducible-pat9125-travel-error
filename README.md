# PAT9125EL reports less travel than actually occurred

Minimum reproducible case. Two variants, one per part number — pick the one that
matches your chip. Single file each, no libraries, builds in the Arduino IDE or
PlatformIO.

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
| Target | ~2 mm rod, sliding axially |

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

Output is CSV (`ms,dx,dy,x_counts,y_counts,x_mm,y_mm`). **`x_counts` is the primary
data** — `x_mm` applies the nominal 50.2 counts/mm, and that scale factor is part of
what is in question.

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
