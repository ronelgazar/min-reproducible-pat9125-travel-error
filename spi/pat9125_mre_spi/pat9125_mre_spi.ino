// =============================================================================
// pat9125_mre.ino — Minimum reproducible case: PAT9125EL under-reports travel
// -----------------------------------------------------------------------------
// Single file, no libraries. Drops into the Arduino IDE or PlatformIO as-is.
// One sensor, 3-wire SPI, bit-banged. Prints CSV so the result can be pasted
// into a spreadsheet directly.
//
// THE PROBLEM
//   Moving the target a ruler-measured distance, the sensor consistently reports
//   LESS travel than actually occurred -- and the shortfall grows with speed:
//       ~3.7 mm/s  ->  sensor reads ~91-92% of true travel
//      ~12.4 mm/s  ->  sensor reads ~88%
//   Repeatable to +/-0.3% within a speed, across many runs and both of two sensors.
//
// SETUP IT WAS SEEN ON
//   PAT9125EL-TKMT (SPI variant), 3-wire SPI, RES_X = RES_Y = 0xFF (1275 cpi,
//   nominal 50.2 counts/mm). Target is a ~2 mm diameter rod sliding axially.
//
// WIRING (any MCU; pins below are Seeed XIAO SAMD21 defaults)
//   SCLK -> D2        shared clock
//   SDIO -> D7        shared bidirectional data (needs a pull-up to 3V3)
//   NCS  -> D1        chip select, active low
//   VDD  -> 3.3 V (2.1-3.6 V)      VLD -> 3.3 V (2.7-3.6 V)      VSS -> GND
//
// HOW TO REPRODUCE
//   1. Flash. Confirm it prints "product_id=0x31" -- anything else is a wiring
//      or power fault, not this problem.
//   2. Send 'z' to zero the counters.
//   3. Move the target a known distance (ruler/caliper/leadscrew), at a steady,
//      known speed.
//   4. Compare the printed x_mm against the true distance.
//      Repeat at a faster speed and compare the ratio.
//
// COMMANDS:  z = zero counters    ? = print status
// =============================================================================
#include <Arduino.h>

// ---- pins -------------------------------------------------------------------
static const uint8_t PIN_SCLK = 2;
static const uint8_t PIN_SDIO = 7;
static const uint8_t PIN_NCS  = 1;

// ---- bus timing -------------------------------------------------------------
static const uint8_t HALF_US = 2;   // ~250 kHz SCLK (chip max is 2 MHz)
static const uint8_t TURN_US = 3;   // SDIO turnaround before the slave drives

// ---- registers (AN01 4.0) ---------------------------------------------------
static const uint8_t REG_PRODUCT_ID = 0x00;   // reads 0x31
static const uint8_t REG_MOTION     = 0x02;   // bit7 = motion available
static const uint8_t REG_DELTA_X_LO = 0x03;
static const uint8_t REG_DELTA_Y_LO = 0x04;
static const uint8_t REG_CONFIG     = 0x06;   // 0x97 reset, 0x17 run
static const uint8_t REG_WRITE_PROT = 0x09;   // 0x5A unlock, 0x00 lock
static const uint8_t REG_RES_X      = 0x0D;
static const uint8_t REG_RES_Y      = 0x0E;
static const uint8_t REG_DELTA_XY_H = 0x12;   // [7:4]=X[11:8], [3:0]=Y[11:8]
static const uint8_t REG_ORIENTATION= 0x19;   // 0x04 = 12-bit data format
static const uint8_t REG_BANK_SEL   = 0x7F;   // 0x00 = bank 0 (write-only)

static const uint8_t PRODUCT_ID_EXPECTED = 0x31;
static const uint8_t MOTION_BIT          = 0x80;

// Nominal scale at RES = 0xFF: 255 * 5 cpi = 1275 cpi; / 25.4 = 50.2 counts/mm.
// NOMINAL ONLY -- the true figure varies with standoff and must be measured.
static const float COUNTS_PER_MM = 50.2f;

// ---- bit-banged 3-wire SPI --------------------------------------------------
// Address byte: bit7 = R/W (1 = write, 0 = read). Data latched on the trailing
// clock edge. SCLK idles HIGH.
static void busBegin() {
  pinMode(PIN_NCS, OUTPUT);  digitalWrite(PIN_NCS, HIGH);
  pinMode(PIN_SCLK, OUTPUT); digitalWrite(PIN_SCLK, HIGH);
  pinMode(PIN_SDIO, INPUT);                       // high-Z until driven
}

static void shiftOut8(uint8_t b) {
  for (int i = 0; i < 8; ++i) {
    digitalWrite(PIN_SDIO, (b & (0x80 >> i)) ? HIGH : LOW);
    digitalWrite(PIN_SCLK, LOW);  delayMicroseconds(HALF_US);
    digitalWrite(PIN_SCLK, HIGH); delayMicroseconds(HALF_US);
  }
}

static uint8_t shiftIn8() {
  uint8_t v = 0;
  for (int i = 0; i < 8; ++i) {
    digitalWrite(PIN_SCLK, LOW);  delayMicroseconds(HALF_US);
    digitalWrite(PIN_SCLK, HIGH); delayMicroseconds(HALF_US);
    v = (uint8_t)((v << 1) | (digitalRead(PIN_SDIO) ? 1 : 0));
  }
  return v;
}

static void regWrite(uint8_t reg, uint8_t value) {
  pinMode(PIN_SDIO, OUTPUT);
  digitalWrite(PIN_SCLK, HIGH);
  digitalWrite(PIN_NCS, LOW);   delayMicroseconds(1);
  shiftOut8((uint8_t)(reg | 0x80));
  shiftOut8(value);
  delayMicroseconds(1);
  digitalWrite(PIN_NCS, HIGH);
  pinMode(PIN_SDIO, INPUT);     delayMicroseconds(1);
}

static uint8_t regRead(uint8_t reg) {
  pinMode(PIN_SDIO, OUTPUT);
  digitalWrite(PIN_SCLK, HIGH);
  digitalWrite(PIN_NCS, LOW);   delayMicroseconds(1);
  shiftOut8((uint8_t)(reg & 0x7F));
  pinMode(PIN_SDIO, INPUT);     delayMicroseconds(TURN_US);   // turnaround
  uint8_t v = shiftIn8();
  delayMicroseconds(1);
  digitalWrite(PIN_NCS, HIGH);  delayMicroseconds(1);
  return v;
}

// ---- sensor -----------------------------------------------------------------
static int32_t cumX = 0, cumY = 0;      // accumulated counts since last zero
static uint32_t motionReads = 0;

static bool sensorInit() {
  uint8_t pid = regRead(REG_PRODUCT_ID);
  Serial.print("product_id=0x"); Serial.println(pid, HEX);
  if (pid != PRODUCT_ID_EXPECTED) {
    Serial.println("FAIL: expected 0x31. 0x00/0xFF => wiring or power, not this bug.");
    return false;
  }
  regWrite(REG_BANK_SEL, 0x00);        // bank 0
  regWrite(REG_CONFIG,   0x97);        // soft reset (self-clearing)
  delay(1);
  regWrite(REG_CONFIG,   0x17);        // leave reset
  regWrite(REG_WRITE_PROT, 0x5A);      // unlock
  regWrite(REG_RES_X, 0xFF);           // 1275 cpi
  regWrite(REG_RES_Y, 0xFF);
  regWrite(REG_ORIENTATION, 0x04);     // 12-bit data format
  regWrite(REG_WRITE_PROT, 0x00);      // re-lock
  // read back the two that matter, so a silent write failure cannot hide
  Serial.print("res_x=0x");  Serial.print(regRead(REG_RES_X), HEX);
  Serial.print(" res_y=0x"); Serial.println(regRead(REG_RES_Y), HEX);
  return true;
}

static int16_t signExtend12(uint16_t v) {       // 12-bit two's complement
  return (int16_t)((v & 0x800) ? (int16_t)(v | 0xF000) : (int16_t)v);
}

// Returns true if motion was present. Reads only when the motion bit is set --
// the chip accumulates between reads, so polling rate is not critical.
static bool readMotion(int16_t &dx, int16_t &dy) {
  uint8_t motion = regRead(REG_MOTION);
  if (!(motion & MOTION_BIT)) { dx = dy = 0; return false; }
  uint8_t xl = regRead(REG_DELTA_X_LO);
  uint8_t yl = regRead(REG_DELTA_Y_LO);
  uint8_t hi = regRead(REG_DELTA_XY_H);
  dx = signExtend12((uint16_t)(((uint16_t)(hi & 0xF0) << 4) | xl));
  dy = signExtend12((uint16_t)(((uint16_t)(hi & 0x0F) << 8) | yl));
  return true;
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) {}
  delay(50);                            // >= 10 ms power-on (DS Table 5)
  busBegin();
  Serial.println("# pat9125 minimum reproducible case");
  if (!sensorInit()) { Serial.println("# init failed -- halted"); while (1) delay(1000); }
  Serial.print("# counts_per_mm (nominal) = "); Serial.println(COUNTS_PER_MM, 2);
  Serial.println("# commands: z = zero, ? = status");
  Serial.println("ms,dx,dy,x_counts,y_counts,x_mm,y_mm");
}

void loop() {
  if (Serial.available()) {
    char c = (char)Serial.read();
    if (c == 'z') { cumX = cumY = 0; motionReads = 0; Serial.println("# zeroed"); }
    else if (c == '?') {
      Serial.print("# pid=0x"); Serial.print(regRead(REG_PRODUCT_ID), HEX);
      Serial.print(" motion_reads="); Serial.println(motionReads);
    }
  }

  int16_t dx, dy;
  if (readMotion(dx, dy)) {
    cumX += dx; cumY += dy; ++motionReads;
  }

  // Print at a fixed 50 Hz regardless of poll rate, so the log stays readable.
  static uint32_t lastPrint = 0;
  uint32_t now = millis();
  if (now - lastPrint >= 20) {
    lastPrint = now;
    Serial.print(now);       Serial.print(',');
    Serial.print(dx);        Serial.print(',');
    Serial.print(dy);        Serial.print(',');
    Serial.print(cumX);      Serial.print(',');
    Serial.print(cumY);      Serial.print(',');
    Serial.print(cumX / COUNTS_PER_MM, 3); Serial.print(',');
    Serial.println(cumY / COUNTS_PER_MM, 3);
  }
}
