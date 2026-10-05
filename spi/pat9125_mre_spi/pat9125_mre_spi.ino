// =============================================================================
// pat9125_mre_spi.ino — Minimum reproducible case: PAT9125EL under-reports travel
//                       3-wire SPI variant (PAT9125EL-TKMT), TWO sensors
// -----------------------------------------------------------------------------
// Single file, no libraries. Arduino IDE or PlatformIO, as-is.
// Two sensors on one shared bus, as in the rig where this was observed -- both
// report the same shortfall, which is why a single bad part is not the explanation.
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
//   SCLK -> D2     shared clock          -> both sensors
//   SDIO -> D7     shared bidirectional  -> both sensors (pull-up to 3V3)
//   NCS1 -> D1     sensor 1 chip select only
//   NCS2 -> D0     sensor 2 chip select only
//   VDD  -> 3.3 V (2.1-3.6 V)   VLD -> 3.3 V (2.7-3.6 V)   VSS -> GND
//
//   Both NCS idle HIGH; exactly one is pulled LOW per transaction.
//
// HOW TO REPRODUCE
//   1. Flash. Both sensors must report product_id=0x31.
//      0x00/0xFF is a wiring or power fault, not this problem.
//   2. Send 'z' to zero the counters.
//   3. Move the target a known distance (ruler/caliper/leadscrew) at a steady,
//      known speed, past both sensors.
//   4. Compare each printed *_x_mm against the true distance.
//   5. Repeat faster and compare the ratios.
//
// COMMANDS:  z = zero    ? = status
// =============================================================================
#include <Arduino.h>

static const uint8_t PIN_SCLK = 2;              // shared
static const uint8_t PIN_SDIO = 7;              // shared
static const uint8_t NCS_PIN[2] = { 1, 0 };     // sensor 1, sensor 2
static const uint8_t N_SENSORS = 2;

static const uint8_t HALF_US = 2;   // ~250 kHz SCLK (chip max 2 MHz)
static const uint8_t TURN_US = 3;   // SDIO turnaround before the slave drives

static const uint8_t REG_PRODUCT_ID = 0x00, REG_MOTION = 0x02,
                     REG_DELTA_X_LO = 0x03, REG_DELTA_Y_LO = 0x04,
                     REG_CONFIG = 0x06, REG_WRITE_PROT = 0x09,
                     REG_RES_X = 0x0D, REG_RES_Y = 0x0E,
                     REG_DELTA_XY_H = 0x12, REG_ORIENTATION = 0x19,
                     REG_BANK_SEL = 0x7F;
static const uint8_t PRODUCT_ID_EXPECTED = 0x31, MOTION_BIT = 0x80;

// Nominal at RES=0xFF: 255*5 = 1275 cpi; /25.4 = 50.2 counts/mm.
// NOMINAL ONLY -- varies with standoff; the raw counts are the real data.
static const float COUNTS_PER_MM = 50.2f;

// ---- bit-banged 3-wire SPI --------------------------------------------------
// Address byte: bit7 = R/W (1 = write, 0 = read). Data latched on the trailing
// clock edge. SCLK idles HIGH. Only the addressed sensor's NCS goes low.
static void busBegin(){
  for (uint8_t i = 0; i < N_SENSORS; ++i){
    pinMode(NCS_PIN[i], OUTPUT); digitalWrite(NCS_PIN[i], HIGH);   // deselected
  }
  pinMode(PIN_SCLK, OUTPUT); digitalWrite(PIN_SCLK, HIGH);
  pinMode(PIN_SDIO, INPUT);                                        // high-Z
}
static void shiftOut8(uint8_t b){
  for (int i = 0; i < 8; ++i){
    digitalWrite(PIN_SDIO, (b & (0x80 >> i)) ? HIGH : LOW);
    digitalWrite(PIN_SCLK, LOW);  delayMicroseconds(HALF_US);
    digitalWrite(PIN_SCLK, HIGH); delayMicroseconds(HALF_US);
  }
}
static uint8_t shiftIn8(){
  uint8_t v = 0;
  for (int i = 0; i < 8; ++i){
    digitalWrite(PIN_SCLK, LOW);  delayMicroseconds(HALF_US);
    digitalWrite(PIN_SCLK, HIGH); delayMicroseconds(HALF_US);
    v = (uint8_t)((v << 1) | (digitalRead(PIN_SDIO) ? 1 : 0));
  }
  return v;
}
static void regWrite(uint8_t s, uint8_t reg, uint8_t value){
  pinMode(PIN_SDIO, OUTPUT);
  digitalWrite(PIN_SCLK, HIGH);
  digitalWrite(NCS_PIN[s], LOW); delayMicroseconds(1);
  shiftOut8((uint8_t)(reg | 0x80)); shiftOut8(value);
  delayMicroseconds(1);
  digitalWrite(NCS_PIN[s], HIGH);
  pinMode(PIN_SDIO, INPUT); delayMicroseconds(1);
}
static uint8_t regRead(uint8_t s, uint8_t reg){
  pinMode(PIN_SDIO, OUTPUT);
  digitalWrite(PIN_SCLK, HIGH);
  digitalWrite(NCS_PIN[s], LOW); delayMicroseconds(1);
  shiftOut8((uint8_t)(reg & 0x7F));
  pinMode(PIN_SDIO, INPUT); delayMicroseconds(TURN_US);   // turnaround
  uint8_t v = shiftIn8();
  delayMicroseconds(1);
  digitalWrite(NCS_PIN[s], HIGH); delayMicroseconds(1);
  return v;
}

// ---- sensors ----------------------------------------------------------------
static int32_t  cumX[2] = {0, 0}, cumY[2] = {0, 0};
static uint32_t motionReads[2] = {0, 0};
static bool     alive[2] = {false, false};

static bool sensorInit(uint8_t s){
  uint8_t pid = regRead(s, REG_PRODUCT_ID);
  Serial.print("# sensor "); Serial.print(s + 1);
  Serial.print(" (NCS=D");   Serial.print(NCS_PIN[s]);
  Serial.print(") product_id=0x"); Serial.println(pid, HEX);
  if (pid != PRODUCT_ID_EXPECTED){
    Serial.println("#   expected 0x31. 0x00/0xFF => wiring or power, not this bug.");
    return false;
  }
  regWrite(s, REG_BANK_SEL, 0x00);          // bank 0
  regWrite(s, REG_CONFIG,   0x97); delay(1); // soft reset (self-clearing)
  regWrite(s, REG_CONFIG,   0x17);          // leave reset
  regWrite(s, REG_WRITE_PROT, 0x5A);        // unlock
  regWrite(s, REG_RES_X, 0xFF);             // 1275 cpi
  regWrite(s, REG_RES_Y, 0xFF);
  regWrite(s, REG_ORIENTATION, 0x04);       // 12-bit data format
  regWrite(s, REG_WRITE_PROT, 0x00);        // re-lock
  // read back, so a silent write failure cannot masquerade as a scale error
  Serial.print("#   res_x=0x");  Serial.print(regRead(s, REG_RES_X), HEX);
  Serial.print(" res_y=0x");     Serial.println(regRead(s, REG_RES_Y), HEX);
  return true;
}

static int16_t signExtend12(uint16_t v){
  return (int16_t)((v & 0x800) ? (int16_t)(v | 0xF000) : (int16_t)v);
}
// The chip accumulates between reads and a read clears the registers, so the
// polling rate is not critical -- see "already ruled out" in the README.
static bool readMotion(uint8_t s, int16_t &dx, int16_t &dy){
  uint8_t motion = regRead(s, REG_MOTION);
  if (!(motion & MOTION_BIT)){ dx = dy = 0; return false; }
  uint8_t xl = regRead(s, REG_DELTA_X_LO);
  uint8_t yl = regRead(s, REG_DELTA_Y_LO);
  uint8_t hi = regRead(s, REG_DELTA_XY_H);
  dx = signExtend12((uint16_t)(((uint16_t)(hi & 0xF0) << 4) | xl));
  dy = signExtend12((uint16_t)(((uint16_t)(hi & 0x0F) << 8) | yl));
  return true;
}

void setup(){
  Serial.begin(115200);
  while (!Serial && millis() < 3000) {}
  delay(50);                      // >= 10 ms power-on (DS Table 5)
  busBegin();
  Serial.println("# pat9125 minimum reproducible case -- 3-wire SPI, 2 sensors");
  for (uint8_t s = 0; s < N_SENSORS; ++s) alive[s] = sensorInit(s);
  if (!alive[0] && !alive[1]){ Serial.println("# no sensor responded -- halted"); while (1) delay(1000); }
  if (!alive[0] || !alive[1])  Serial.println("# WARNING: only one sensor is responding");
  Serial.print("# counts_per_mm (nominal) = "); Serial.println(COUNTS_PER_MM, 2);
  Serial.println("# commands: z = zero, ? = status");
  Serial.println("ms,s1_dx,s1_dy,s1_x,s1_y,s1_x_mm,s2_dx,s2_dy,s2_x,s2_y,s2_x_mm");
}

void loop(){
  if (Serial.available()){
    char c = (char)Serial.read();
    if (c == 'z'){
      for (uint8_t s = 0; s < N_SENSORS; ++s){ cumX[s] = cumY[s] = 0; motionReads[s] = 0; }
      Serial.println("# zeroed");
    } else if (c == '?'){
      for (uint8_t s = 0; s < N_SENSORS; ++s){
        Serial.print("# s"); Serial.print(s + 1);
        Serial.print(" pid=0x"); Serial.print(regRead(s, REG_PRODUCT_ID), HEX);
        Serial.print(" motion_reads="); Serial.println(motionReads[s]);
      }
    }
  }

  int16_t dx[2] = {0, 0}, dy[2] = {0, 0};
  for (uint8_t s = 0; s < N_SENSORS; ++s){
    if (!alive[s]) continue;
    if (readMotion(s, dx[s], dy[s])){ cumX[s] += dx[s]; cumY[s] += dy[s]; ++motionReads[s]; }
  }

  // Fixed 50 Hz print, independent of poll rate, so the log stays readable.
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
    }
    Serial.println();
  }
}
