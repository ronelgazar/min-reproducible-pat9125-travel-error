// =============================================================================
// pat9125_mre_i2c.ino — Minimum reproducible case: PAT9125EL under-reports travel
//                       I2C variant (PAT9125EL-TKIT), TWO sensors
// -----------------------------------------------------------------------------
// Single file, no libraries. Arduino IDE or PlatformIO, as-is.
// Two sensors on one shared bus, as in the rig where this was observed -- both
// report the same shortfall, which is why a single bad part is not the explanation.
//
// If your part is the SPI variant (PAT9125EL-TKMT) use ../../spi instead. The two
// part numbers are NOT interchangeable (AN01 1.1). This sketch scans every
// documented address, so if nothing answers you are probably holding the other one.
//
// THE PROBLEM
//   Moving the target a ruler-measured distance, BOTH sensors consistently report
//   LESS travel than actually occurred -- and the shortfall grows with speed:
//       ~3.7 mm/s  ->  ~91-92% of true travel
//      ~12.4 mm/s  ->  ~88%
//   Repeatable to +/-0.3% within a speed, over many runs, on both sensors.
//
// SETUP IT WAS SEEN ON
//   RES_X = RES_Y = 0xFF (1275 cpi, nominal 50.2 counts/mm), ORIENTATION = 0x04.
//   Target is a ~2 mm diameter rod sliding axially past both sensors.
//
// WIRING (any MCU; pins below are Seeed XIAO SAMD21 defaults)
//   SCL -> D7    shared clock -> both sensors, pull-up to 3V3 (AN01 Fig.1: 5k)
//   SDA -> D8    shared data  -> both sensors, pull-up to 3V3 (AN01 Fig.1: 5k)
//   ID_SEL per sensor picks its address: GND = 0x75, VDD = 0x73, floating = 0x79.
//     Two sensors on one bus therefore need DIFFERENT ID_SEL levels.
//   VDD -> 3.3 V (2.1-3.6 V)   VLD -> 3.3 V (2.7-3.6 V)   VSS -> GND
//
//   BOTH lines need pull-ups. A missing pull-up on SCL is a common cause of
//   every read returning 0xFF.
//
// HOW TO REPRODUCE
//   1. Flash. It should find two sensors at two different addresses.
//      All-0xFF everywhere is a wiring/pull-up/power fault, not this problem.
//   2. Send 'z' to zero the counters.
//   3. Move the target a known distance (ruler/caliper/leadscrew) at a steady,
//      known speed, past both sensors.
//   4. Compare each printed *_x_mm against the true distance.
//   5. Repeat faster and compare the ratios.
//
// COMMANDS:  z = zero    ? = status
// =============================================================================
#include <Arduino.h>

static const uint8_t PIN_SCL = 7;    // shared
static const uint8_t PIN_SDA = 8;    // shared

// 1 us half period ~= 500 kHz. The datasheet caps I2C at 400 kHz (Table 5), so
// raise this to 2 for an in-spec ~250 kHz if reads prove unreliable.
static const uint8_t HALF_US = 1;

static const uint8_t REG_PRODUCT_ID = 0x00, REG_MOTION = 0x02,
                     REG_DELTA_X_LO = 0x03, REG_DELTA_Y_LO = 0x04,
                     REG_CONFIG = 0x06, REG_WRITE_PROT = 0x09,
                     REG_RES_X = 0x0D, REG_RES_Y = 0x0E,
                     REG_DELTA_XY_H = 0x12, REG_ORIENTATION = 0x19,
                     REG_BANK_SEL = 0x7F,
                     // Optical diagnostics. Shutter is the exposure index: the chip
                     // lengthens it when the image is dim, and a longer exposure means
                     // more motion blur per frame -- which is how poor illumination
                     // turns into LOST COUNTS AT SPEED. Frame_Avg is mean image
                     // brightness. Read them while stationary and while moving, slow
                     // and fast: a shutter that climbs with speed, or a low Frame_Avg,
                     // points at illumination/aperture rather than the mechanics.
                     REG_SHUTTER = 0x14, REG_FRAME_AVG = 0x17;
static const uint8_t PRODUCT_ID_EXPECTED = 0x31, MOTION_BIT = 0x80;

static const uint8_t CANDIDATES[] = { 0x75, 0x73, 0x79 };   // ID_SEL: GND/VDD/float
static const uint8_t N_SENSORS = 2;
static uint8_t addr[2]  = { 0x00, 0x00 };                   // filled by the scan
static bool    alive[2] = { false, false };

static const float COUNTS_PER_MM = 50.2f;   // NOMINAL; raw counts are the real data

// ---- bit-banged I2C ---------------------------------------------------------
// Open-drain: never drive a line high. Drive LOW, or release and let the
// external pull-up raise it -- driving high would fight the pull-up and any
// slave holding the line.
static void sdaHigh(){ pinMode(PIN_SDA, INPUT); }
static void sdaLow (){ pinMode(PIN_SDA, OUTPUT); digitalWrite(PIN_SDA, LOW); }
static void sclHigh(){ pinMode(PIN_SCL, INPUT); }
static void sclLow (){ pinMode(PIN_SCL, OUTPUT); digitalWrite(PIN_SCL, LOW); }
static void busBegin(){ sdaHigh(); sclHigh(); delayMicroseconds(500); }

static void i2cStart(){
  sdaHigh(); sclHigh(); delayMicroseconds(HALF_US);
  sdaLow();  delayMicroseconds(HALF_US);
  sclLow();  delayMicroseconds(HALF_US);
}
static void i2cStop(){
  sdaLow();  delayMicroseconds(HALF_US);
  sclHigh(); delayMicroseconds(HALF_US);
  sdaHigh(); delayMicroseconds(HALF_US);
}
static bool writeByte(uint8_t b){
  for (int i = 0; i < 8; ++i){
    (b & 0x80) ? sdaHigh() : sdaLow();  b <<= 1;
    delayMicroseconds(HALF_US);
    sclHigh(); delayMicroseconds(HALF_US);
    sclLow();
  }
  sdaHigh(); delayMicroseconds(HALF_US);        // release for the slave's ACK
  sclHigh(); delayMicroseconds(HALF_US);
  bool ack = (digitalRead(PIN_SDA) == 0);
  sclLow();  delayMicroseconds(HALF_US);
  return ack;
}
static uint8_t readByte(bool ack){
  uint8_t v = 0; sdaHigh();
  for (int i = 0; i < 8; ++i){
    delayMicroseconds(HALF_US);
    sclHigh(); delayMicroseconds(HALF_US);
    v = (uint8_t)((v << 1) | (digitalRead(PIN_SDA) ? 1 : 0));
    sclLow();
  }
  ack ? sdaLow() : sdaHigh(); delayMicroseconds(HALF_US);
  sclHigh(); delayMicroseconds(HALF_US);
  sclLow();  sdaHigh();
  return v;
}
static bool regReadAt(uint8_t a, uint8_t reg, uint8_t &value){
  i2cStart();
  if (!writeByte((uint8_t)(a << 1)))       { i2cStop(); return false; }
  if (!writeByte(reg))                     { i2cStop(); return false; }
  i2cStart();                                               // repeated start
  if (!writeByte((uint8_t)((a << 1) | 1))) { i2cStop(); return false; }
  value = readByte(false);                                  // NACK single byte
  i2cStop(); return true;
}
static bool regWriteAt(uint8_t a, uint8_t reg, uint8_t value){
  i2cStart();
  if (!writeByte((uint8_t)(a << 1))) { i2cStop(); return false; }
  if (!writeByte(reg))               { i2cStop(); return false; }
  if (!writeByte(value))             { i2cStop(); return false; }
  i2cStop(); return true;
}
static uint8_t rd(uint8_t s, uint8_t reg){ uint8_t v = 0xFF; regReadAt(addr[s], reg, v); return v; }
static void    wr(uint8_t s, uint8_t reg, uint8_t v){ regWriteAt(addr[s], reg, v); }

// ---- sensors ----------------------------------------------------------------
static int32_t  cumX[2] = {0, 0}, cumY[2] = {0, 0};
static uint32_t motionReads[2] = {0, 0};

// Scan every documented address and keep the ones that answer 0x31. Two sensors
// on one bus must be strapped to different ID_SEL levels, so they land on
// different addresses -- if only one appears, both are likely strapped the same.
static uint8_t scanBus(){
  pinMode(PIN_SDA, INPUT_PULLUP); pinMode(PIN_SCL, INPUT_PULLUP);
  delayMicroseconds(500);
  Serial.print("# idle lines: SDA="); Serial.print(digitalRead(PIN_SDA) ? "HIGH" : "LOW");
  Serial.print(" SCL=");              Serial.println(digitalRead(PIN_SCL) ? "HIGH" : "LOW");
  Serial.println("#   both HIGH is correct; a LOW line is held down and nothing will answer");
  busBegin();

  uint8_t found = 0;
  for (uint8_t i = 0; i < sizeof(CANDIDATES) && found < N_SENSORS; ++i){
    uint8_t a = CANDIDATES[i], pid = 0xFF;
    bool ok = regReadAt(a, REG_PRODUCT_ID, pid);
    Serial.print("# probe 0x"); Serial.print(a, HEX);
    Serial.print(" -> ");       Serial.print(ok ? "ACK " : "no ACK ");
    Serial.print("pid=0x");     Serial.println(pid, HEX);
    if (ok && pid == PRODUCT_ID_EXPECTED){ addr[found] = a; alive[found] = true; ++found; }
  }
  Serial.print("# sensors found: "); Serial.println(found);
  if (found == 1) Serial.println("# WARNING: only one. Two sensors need DIFFERENT ID_SEL levels.");
  return found;
}

static bool sensorInit(uint8_t s){
  if (!alive[s]) return false;
  wr(s, REG_BANK_SEL, 0x00);
  wr(s, REG_CONFIG,   0x97); delay(1);
  wr(s, REG_CONFIG,   0x17);
  wr(s, REG_WRITE_PROT, 0x5A);
  wr(s, REG_RES_X, 0xFF);
  wr(s, REG_RES_Y, 0xFF);
  wr(s, REG_ORIENTATION, 0x04);
  wr(s, REG_WRITE_PROT, 0x00);
  // read back, so a silent write failure cannot masquerade as a scale error
  Serial.print("# sensor "); Serial.print(s + 1);
  Serial.print(" @0x");      Serial.print(addr[s], HEX);
  Serial.print(" res_x=0x"); Serial.print(rd(s, REG_RES_X), HEX);
  Serial.print(" res_y=0x"); Serial.println(rd(s, REG_RES_Y), HEX);
  return true;
}

static int16_t signExtend12(uint16_t v){
  return (int16_t)((v & 0x800) ? (int16_t)(v | 0xF000) : (int16_t)v);
}
// The chip accumulates between reads and a read clears the registers, so the
// polling rate is not critical -- see "already ruled out" in the README.
static bool readMotion(uint8_t s, int16_t &dx, int16_t &dy){
  uint8_t motion = 0;
  if (!regReadAt(addr[s], REG_MOTION, motion)){ dx = dy = 0; return false; }
  if (!(motion & MOTION_BIT))                 { dx = dy = 0; return false; }
  uint8_t xl = 0, yl = 0, hi = 0;
  if (!regReadAt(addr[s], REG_DELTA_X_LO, xl) ||
      !regReadAt(addr[s], REG_DELTA_Y_LO, yl) ||
      !regReadAt(addr[s], REG_DELTA_XY_H,  hi)){ dx = dy = 0; return false; }
  dx = signExtend12((uint16_t)(((uint16_t)(hi & 0xF0) << 4) | xl));
  dy = signExtend12((uint16_t)(((uint16_t)(hi & 0x0F) << 8) | yl));
  return true;
}


// ---- guided optical test -----------------------------------------------------
// Three phases, advanced with 't': REST, SLOW, FAST. Each samples Shutter and
// Frame_Avg continuously, then the result is interpreted at the end.
//
// The logic being tested: if the image is dim the chip lengthens its exposure,
// a longer exposure blurs more per frame, and more blur at higher speed loses
// counts. So a Shutter that CLIMBS with speed, or a persistently low Frame_Avg,
// implicates illumination/aperture. Flat readings across all three phases
// exonerate it and send you back to mechanics or scale.
// NOTE: signatures below take uint8_t, not Phase -- the Arduino .ino
// preprocessor hoists auto-generated prototypes above this declaration.
enum Phase { PH_IDLE = 0, PH_REST = 1, PH_SLOW = 2, PH_FAST = 3, PH_DONE = 4 };
static Phase phase = PH_IDLE;
static const char* PHASE_NAME[] = { "idle", "REST", "SLOW", "FAST", "done" };

struct PhaseStat {
  uint32_t n; uint32_t shSum, faSum;
  uint8_t  shMin, shMax, faMin, faMax;
  int32_t  cumStart, cumEnd; uint32_t msStart, msEnd;
};
static PhaseStat ps[5];        // indexed by Phase

static void phaseReset(uint8_t p){
  PhaseStat &q = ps[p];
  q.n = q.shSum = q.faSum = 0;
  q.shMin = q.faMin = 255; q.shMax = q.faMax = 0;
  q.cumStart = cumX[0]; q.cumEnd = cumX[0];
  q.msStart = millis(); q.msEnd = q.msStart;
}
static void phaseSample(uint8_t p, uint8_t sh, uint8_t fa){
  PhaseStat &q = ps[p];
  q.shSum += sh; q.faSum += fa; ++q.n;
  if (sh < q.shMin) q.shMin = sh;  if (sh > q.shMax) q.shMax = sh;
  if (fa < q.faMin) q.faMin = fa;  if (fa > q.faMax) q.faMax = fa;
  q.cumEnd = cumX[0]; q.msEnd = millis();
}
static float phaseSpeed(uint8_t p){
  PhaseStat &q = ps[p];
  float sec = (q.msEnd - q.msStart) / 1000.0f;
  if (sec <= 0.05f) return 0.0f;
  return fabs((q.cumEnd - q.cumStart) / COUNTS_PER_MM) / sec;
}
static float pct(float from, float to){ return (from > 0.01f) ? (to - from) / from * 100.0f : 0.0f; }

static void phaseRow(uint8_t p){
  PhaseStat &q = ps[p];
  if (!q.n){ Serial.print("  "); Serial.print(PHASE_NAME[p]); Serial.println("  (no samples)"); return; }
  Serial.print("  "); Serial.print(PHASE_NAME[p]);
  Serial.print("\tspeed="); Serial.print(phaseSpeed(p), 2); Serial.print(" mm/s");
  Serial.print("\tshutter ");  Serial.print(q.shMin); Serial.print("/");
  Serial.print((float)q.shSum / q.n, 1);                Serial.print("/");
  Serial.print(q.shMax);
  Serial.print("\tframe_avg "); Serial.print(q.faMin); Serial.print("/");
  Serial.print((float)q.faSum / q.n, 1);                Serial.print("/");
  Serial.println(q.faMax);
}

static void testReport(){
  Serial.println();
  Serial.println("=== OPTICAL DIAGNOSTIC (sensor 1) ===");
  Serial.println("  phase\tspeed\t\tshutter min/mean/max\tframe_avg min/mean/max");
  phaseRow(PH_REST); phaseRow(PH_SLOW); phaseRow(PH_FAST);

  if (!ps[PH_REST].n || !ps[PH_FAST].n){
    Serial.println("  (need REST and FAST phases for a verdict)"); return;
  }
  float shRest = (float)ps[PH_REST].shSum / ps[PH_REST].n;
  float shFast = (float)ps[PH_FAST].shSum / ps[PH_FAST].n;
  float faRest = (float)ps[PH_REST].faSum / ps[PH_REST].n;
  float faFast = (float)ps[PH_FAST].faSum / ps[PH_FAST].n;
  float dSh = pct(shRest, shFast), dFa = pct(faRest, faFast);

  Serial.println();
  Serial.print("  shutter   rest->fast: "); Serial.print(dSh, 1); Serial.println("%");
  Serial.print("  frame_avg rest->fast: "); Serial.print(dFa, 1); Serial.println("%");
  Serial.println();
  Serial.println("  INTERPRETATION (heuristic -- the numbers above are the evidence):");

  bool flagged = false;
  if (dSh > 25.0f){
    flagged = true;
    Serial.println("  * Shutter lengthens markedly under motion. The chip is short of");
    Serial.println("    light and compensating with exposure -- which blurs more per frame");
    Serial.println("    the faster you go. CONSISTENT with an aperture/illumination cause");
    Serial.println("    for the speed-dependent count loss.");
  }
  if (faFast < 25.0f){
    flagged = true;
    Serial.println("  * Frame_Avg is very low: little light reaching the array. Check the");
    Serial.println("    aperture (datasheet 4.5.1 wants > 3.2 x 2.6 mm), standoff, and that");
    Serial.println("    nothing is shadowing the VCSEL.");
  }
  if (faFast > 230.0f){
    flagged = true;
    Serial.println("  * Frame_Avg is near saturation: ambient light is likely swamping the");
    Serial.println("    VCSEL. Shield the opening. NOTE: enlarging the aperture makes this");
    Serial.println("    WORSE, so fix the shielding before drilling.");
  }
  if (fabs(dSh) < 10.0f && faFast > 25.0f && faFast < 230.0f){
    flagged = true;
    Serial.println("  * Shutter and Frame_Avg are both stable and mid-range across all");
    Serial.println("    phases. Illumination is NOT the limiting factor here -- look to");
    Serial.println("    standoff variation, scale calibration, or the mechanics instead.");
  }
  if (!flagged){
    Serial.println("  * Mixed signals -- no single cause stands out. Report the table above.");
  }
  Serial.println("=====================================");
}

static void advancePhase(){
  if (phase == PH_IDLE){
    phase = PH_REST; phaseReset(phase);
    Serial.println("# PHASE 1/3 REST -- hold everything still. Press 't' when ready.");
  } else if (phase == PH_REST){
    phase = PH_SLOW; phaseReset(phase);
    Serial.println("# PHASE 2/3 SLOW -- move the target SLOWLY. Press 't' when done.");
  } else if (phase == PH_SLOW){
    phase = PH_FAST; phaseReset(phase);
    Serial.println("# PHASE 3/3 FAST -- move the target FAST. Press 't' when done.");
  } else if (phase == PH_FAST){
    phase = PH_DONE; testReport(); phase = PH_IDLE;
  }
}

void setup(){
  Serial.begin(115200);
  while (!Serial && millis() < 3000) {}
  delay(50);                      // >= 10 ms power-on (DS Table 5)
  Serial.println("# pat9125 minimum reproducible case -- I2C, 2 sensors");
  if (scanBus() == 0){
    Serial.println("# FAIL: nothing answered 0x31 on 0x75/0x73/0x79.");
    Serial.println("#   all 0xFF -> no device driving: power, wiring, or missing pull-ups");
    Serial.println("#   all 0x00 -> SDA stuck low");
    Serial.println("#   If this is a TKMT (SPI) part, use the ../../spi sketch instead.");
    while (1) delay(1000);
  }
  for (uint8_t s = 0; s < N_SENSORS; ++s) sensorInit(s);
  for (uint8_t s = 0; s < N_SENSORS; ++s){
    if (!alive[s]) continue;
    Serial.print("# sensor "); Serial.print(s + 1);
    Serial.print(" at rest: shutter=");  Serial.print(rd(s, REG_SHUTTER));
    Serial.print(" frame_avg=");         Serial.println(rd(s, REG_FRAME_AVG));
  }
  Serial.print("# counts_per_mm (nominal) = "); Serial.println(COUNTS_PER_MM, 2);
  Serial.println("# commands: z = zero, ? = status, t = run the guided optical test");
  Serial.println("ms,s1_dx,s1_dy,s1_x,s1_y,s1_x_mm,s1_shutter,s1_frame_avg,s2_dx,s2_dy,s2_x,s2_y,s2_x_mm,s2_shutter,s2_frame_avg");
}

void loop(){
  if (Serial.available()){
    char c = (char)Serial.read();
    if (c == 't'){ advancePhase(); }
    else if (c == 'z'){
      for (uint8_t s = 0; s < N_SENSORS; ++s){ cumX[s] = cumY[s] = 0; motionReads[s] = 0; }
      Serial.println("# zeroed");
    } else if (c == '?'){
      for (uint8_t s = 0; s < N_SENSORS; ++s){
        if (!alive[s]) continue;
        Serial.print("# s"); Serial.print(s + 1);
        Serial.print(" @0x"); Serial.print(addr[s], HEX);
        Serial.print(" pid=0x"); Serial.print(rd(s, REG_PRODUCT_ID), HEX);
        Serial.print(" motion_reads="); Serial.println(motionReads[s]);
      }
    }
  }

  int16_t dx[2] = {0, 0}, dy[2] = {0, 0};
  for (uint8_t s = 0; s < N_SENSORS; ++s){
    if (!alive[s]) continue;
    if (readMotion(s, dx[s], dy[s])){ cumX[s] += dx[s]; cumY[s] += dy[s]; ++motionReads[s]; }
  }

  static uint32_t lastPrint = 0;
  uint32_t now = millis();
  if (now - lastPrint >= 20){
    lastPrint = now;
    Serial.print(now);
    for (uint8_t s = 0; s < N_SENSORS; ++s){
      Serial.print(','); Serial.print(dx[s]);
      Serial.print(','); Serial.print(dy[s]);
      Serial.print(','); Serial.print(cumX[s]);
      Serial.print(','); Serial.print(cumY[s]);
      Serial.print(','); Serial.print(cumX[s] / COUNTS_PER_MM, 3);
      Serial.print(','); Serial.print(alive[s] ? rd(s, REG_SHUTTER) : 0);
      Serial.print(','); Serial.print(alive[s] ? rd(s, REG_FRAME_AVG) : 0);
    }
    Serial.println();
  }
}
