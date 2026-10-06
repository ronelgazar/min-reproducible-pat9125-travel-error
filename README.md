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

A PAT9125EL tracking a **~2 mm diameter rod** reports **less** displacement than the
rod actually travels, and the shortfall **grows with speed**. It is repeatable within
a given speed and appears on both sensors, which is why we read it as systematic
rather than noise or a single faulty part.

**Is this expected for this part on a cylindrical target, and if so what standoff
and aperture geometry is required to avoid it?**

Our own figures are omitted here deliberately — they were taken against commanded
motor travel rather than a caliper, and we would rather you generate the numbers on
your own setup than anchor on ours. The sketches below produce them directly.

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
3. Move the target a **caliper-measured** distance at a steady, known speed.
4. Type `r <true mm>` — e.g. `r 40.15`. For each sensor it prints the raw counts,
   the measured distance, the **ratio** of measured to true, and the **implied
   counts/mm** that the measurement gives.

5. Repeat at a faster speed and compare the two ratios — the gap between them is the
   reported effect.

`ratio` is the headline number: below 1 means the sensor is reporting short.
`implied counts/mm` is what the measurement says the scale factor actually is, which
is useful independently of this issue.

### Commands

| | |
|---|---|
| `z` | zero the counters |
| `r <mm>` | ruler check against a measured distance |
| `t` | step through the guided optical test |
| `?` | status |

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

It then prints, per phase, the mean speed and the min/mean/max of Shutter and
Frame_Avg, followed by the percentage change from REST to FAST and an interpretation.

### Reading the result

| Observation | Means | Points at |
|---|---|---|
| Shutter climbs >25% rest→fast | chip is short of light and lengthening exposure; longer exposure blurs more per frame, and blur grows with speed | **aperture / illumination** — consistent with the speed-dependent loss |
| `Frame_Avg` very low (<25) | little light reaching the array | aperture size, standoff, or a shadowed VCSEL |
| `Frame_Avg` near saturation (>230) | ambient light swamping the VCSEL | shielding — **and note enlarging the aperture makes this worse** |
| Both stable and mid-range | illumination is fine | **not** optical — look to standoff variation, scale calibration, or mechanics |

The thresholds are heuristics; the table of numbers is the actual evidence.

### Before changing any hardware

**Take the REST baseline first.** Once the aperture is enlarged or the mount altered,
the comparison is gone — and the before/after is the entire value of the test. Record
the three-phase table while the rig is still in its current state.

**A saturated `Frame_Avg` inverts the obvious fix.** The intuitive response to an
optical problem is to open the aperture up. That is right when the image is *dim*, and
actively wrong when it is *saturated*: a larger opening lets in more ambient light and
makes swamping worse. These two failures look similar from the outside and have
opposite remedies, which is why the test distinguishes them before anything is drilled.
Worth noting the VCSEL is 850 nm, so ordinary daylight near the rig is a real source.

### What follows from each outcome

| Result | Do this |
|---|---|
| Shutter climbs with speed, `Frame_Avg` low | Enlarge the aperture to at least **3.2 × 2.6 mm** (datasheet §4.5.1, offset 1.45/1.75 mm about centre because it includes the VCSEL). Keep the bore short or chamfered so the chip sees a cone rather than a tunnel. |
| `Frame_Avg` near saturation | **Shield the opening from ambient light before enlarging it.** Re-run the test afterwards; the dim-image case may be hiding underneath. |
| Both stable and mid-range | Illumination is not the cause. Go after standoff variation (we measure 0.13–0.2 mm of lateral wander, so radial wander is likely similar) and the scale calibration. |
| Shutter climbs *and* `Frame_Avg` saturated | Contradictory — re-run with the rig shielded, since stray light during only part of the test will produce this. |

Whatever the outcome, the three-phase table is the thing to send on: it is a direct
measurement of the optical conditions, not an inference from the displacement error.

## Sleep / downshift test — a second candidate cause

Press **`w`**, then make the same move several times varying only the pause before
each (~10 s, ~5 s, ~2 s, ~1 s, then immediately). Repeat the set two or three times.
Each move is detected and reported automatically.

**Why this matters.** The datasheet advertises programmable sleep modes and downshift
time: run current 0.7 mA against 25 uA / 10 uA in Sleep1 / Sleep2. A 30-70x current
drop means a much lower frame rate. These are registers `0x05` Operation_Mode,
`0x0A` Sleep1 and `0x0B` Sleep2 — and **AN01's initialisation sequence never touches
them**, so any implementation following it (including this one) runs the factory
defaults `0xA0`, `0x77`, `0x10`.

If the chip has downshifted during an idle, motion resumes while it is still slow,
the surface moves further between frames than correlation can match, and those counts
are lost. **That loss grows with speed — the same signature as an optical problem,
from an entirely different cause.** It is worth excluding before blaming the optics.

The test needs no register writes and no guessed bit definitions: counts falling as
the pause grows implicates sleep; counts flat across idle times rules it out.

| `analyze.py` says | Means |
|---|---|
| counts fall as pause grows | losing counts waking from sleep/downshift |
| counts flat across idle times | sleep is not the cause — look at optics or mechanics |

## Register list

The widely circulated PAT9125EL datasheet **v1.3 has no register list**. Version 1.2
(31 May 2017) does, in §5.0 — 16 registers:

| Addr | Name | Access | Reset | |
|---|---|---|---|---|
| 0x00 | Product_ID1 | RO | 0x31 | |
| 0x01 | Product_ID2 | RO | 0x91 | stronger identity check than 0x00 alone |
| 0x02 | Motion_Status | RO | - | |
| 0x03 | Delta_X_Lo | RO | - | |
| 0x04 | Delta_Y_Lo | RO | - | |
| 0x05 | Operation_Mode | R/W | 0xA0 | **not set by AN01** |
| 0x06 | Configuration | R/W | 0x17 | |
| 0x09 | Write_Protect | R/W | 0x00 | |
| 0x0A | Sleep1 | R/W | 0x77 | **not set by AN01** |
| 0x0B | Sleep2 | R/W | 0x10 | **not set by AN01** |
| 0x0D | RES_X | R/W | 0x14 | 0x14 = 100 cpi, not 1275 |
| 0x0E | RES_Y | R/W | 0x14 | |
| 0x12 | Delta_XY_Hi | RO | - | |
| 0x14 | Shutter | RO | - | index of LASER shutter time |
| 0x17 | Frame_Avg | RO | - | average brightness of a frame |
| 0x19 | Orientation | R/W | 0x04 | |

Two things follow. **There is no frame-capture or raw-pixel register** — so the
surface scan above is the closest available substitute. And **`0x7F` bank select is
absent from this list** despite being used by both AN01 and Prusa's driver, which
proves the published list is incomplete; undocumented registers exist on this part.

Open questions for PixArt: does the PAT9125EL support raw frame capture, and what
are the bit definitions of `0x05`, `0x0A` and `0x0B`?

## Surface scan — what the sensor sees along the rod

Press **`p`** to toggle scan mode, then move slowly along the full stroke.

`Frame_Avg` is one number per frame, not an image, so this is not a picture of the
surface — but it is what the sensor sees *as a signal*, mapped to position. Samples
are emitted **per unit of travel** rather than per unit of time, so the profile has
uniform spatial resolution however fast you move: roughly one sample every 0.1 mm,
limited by the poll rate.

What it distinguishes:

| Pattern | Means |
|---|---|
| Brightness uniform along the stroke | the surface is not the variable — look at geometry (aperture, standoff) |
| A localised dip | something on the **rod** at that position: contamination, a finish change, a defect |
| Brightness modulating smoothly | standoff varying with position — consistent with rod wobble |
| Shutter rising where brightness falls | the chip compensating for a dark stretch with exposure |

`analyze.py` reports the spread and the darkest position, and `--plot` writes a
`_scan.png` of brightness and exposure against position along the rod.

Note there is **no frame-grab on this part** — neither the datasheet nor AN01
documents one, and no undocumented registers are touched here.

## Capturing and analysing a run

The sketch prints to a serial monitor, which is fine for a quick look but leaves
nothing to analyse or send back. Two small tools close that:

```
python3 tools/capture.py                     # auto-detects the port
python3 tools/analyze.py <file>.csv --plot
```

`capture.py` logs everything the board prints — CSV rows *and* the `#` lines, so
ruler checks and optical-test reports end up in the file — while forwarding anything
you type, so `z`, `r 40.15` and `t` still work mid-recording. Needs `pyserial`.

`analyze.py` segments the log into individual moves and reports, per move: distance
per sensor, the **sensor-to-sensor ratio**, mean speed, and mean Shutter/Frame_Avg.
It then compares the slowest and fastest moves and says whether the optical readings
track speed. Numbers are stdlib-only; `--plot` additionally writes travel, exposure
and brightness against time, and needs `matplotlib`.

The sensor-to-sensor ratio is worth watching independently: near 1.0 means both
sensors see the same thing, so a single faulty part is not the explanation.

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
