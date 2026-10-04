// =============================================================================
// pat9125_mre_i2c.ino — Minimum reproducible case: PAT9125EL under-reports travel
//                       I2C variant (PAT9125EL-TKIT)
// -----------------------------------------------------------------------------
// Single file, no libraries. Drops into the Arduino IDE or PlatformIO as-is.
// One sensor, bit-banged I2C. Prints CSV so the result pastes into a spreadsheet.
//
// If your part is the SPI variant (PAT9125EL-TKMT) use ../spi instead. The two
// part numbers are NOT interchangeable (AN01 1.1): TKIT = I2C, TKMT = SPI.
// This sketch scans all three documented addresses, so if nothing answers on any
// of them, you are probably holding the other variant.
//
// THE PROBLEM
//   Moving the target a ruler-measured distance, the sensor consistently reports
//   LESS travel than actually occurred -- and the shortfall grows with speed:
//       ~3.7 mm/s  ->  sensor reads ~91-92% of true travel
//      ~12.4 mm/s  ->  sensor reads ~88%
//   Repeatable to +/-0.3% within a speed, across many runs and both of two sensors.
//
// SETUP IT WAS SEEN ON
//   RES_X = RES_Y = 0xFF (1275 cpi, nominal 50.2 counts/mm).
//   Target is a ~2 mm diameter rod sliding axially.
//
// WIRING (any MCU; pins below are Seeed XIAO SAMD21 defaults)
//   SCL -> D7      clock      -- needs a pull-up to 3V3 (AN01 Fig.1: 5k)
//   SDA -> D8      data       -- needs a pull-up to 3V3 (AN01 Fig.1: 5k)
//   ID_SEL         selects the address: GND = 0x75, VDD = 0x73, floating = 0x79
//   VDD -> 3.3 V (2.1-3.6 V)   VLD -> 3.3 V (2.7-3.6 V)   VSS -> GND
//
//   BOTH lines need pull-ups. A missing pull-up on SCL is a common cause of
//   every read returning 0xFF.
//
// HOW TO REPRODUCE
//   1. Flash. Confirm it prints "found 0x31 at 0x75" (or 0x73/0x79).
//      All-0xFF on every address is a wiring/pull-up/power fault, not this problem.
//   2. Send 'z' to zero the counters.
//   3. Move the target a known distance (ruler/caliper/leadscrew) at a steady,
//      known speed.
//   4. Compare the printed x_mm against the true distance.
//      Repeat at a faster speed and compare the ratio.
//
// COMMANDS:  z = zero counters    ? = print status
// =============================================================================
#include <Arduino.h>

// ---- pins -------------------------------------------------------------------
static const uint8_t PIN_SCL = 7;
static const uint8_t PIN_SDA = 8;

// ---- bus timing -------------------------------------------------------------
// 1 us half period ~= 500 kHz. NOTE: the datasheet caps I2C at 400 kHz
// (Table 5), so raise this to 2 for an in-spec ~250 kHz if reads are unreliable.
static const uint8_t HALF_US = 1;

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

// ID_SEL: GND = 0x75, VDD = 0x73, floating = 0x79 (AN01 Fig.1)
static const uint8_t ADDRESSES[] = { 0x75, 0x73, 0x79 };
static uint8_t addr = 0x00;   // set by the scan at boot

// Nominal scale at RES = 0xFF: 255 * 5 cpi = 1275 cpi; / 25.4 = 50.2 counts/mm.
// NOMINAL ONLY -- the true figure varies with standoff and must be measured.
static const float COUNTS_PER_MM = 50.2f;

// ---- bit-banged I2C ---------------------------------------------------------
// Open-drain: never drive a line high. Drive LOW, or release and let the
// external pull-up raise it. Driving high would fight the pull-up and any
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
  sdaHigh();                                   // release for the slave's ACK
  delayMicroseconds(HALF_US);
  sclHigh(); delayMicroseconds(HALF_US);
  bool ack = (digitalRead(PIN_SDA) == 0);
  sclLow();  delayMicroseconds(HALF_US);
  return ack;
}
static uint8_t readByte(bool ack){
  uint8_t v = 0;
  sdaHigh();
  for (int i = 0; i < 8; ++i){
    delayMicroseconds(HALF_US);
    sclHigh(); delayMicroseconds(HALF_US);
    v = (uint8_t)((v << 1) | (digitalRead(PIN_SDA) ? 1 : 0));
    sclLow();
  }
  ack ? sdaLow() : sdaHigh();                  // ACK = pull low, NACK = release
  delayMicroseconds(HALF_US);
  sclHigh(); delayMicroseconds(HALF_US);
  sclLow();  sdaHigh();
  return v;
}

static bool regRead(uint8_t reg, uint8_t &value){
  i2cStart();
  if (!writeByte((uint8_t)(addr << 1)))        { i2cStop(); return false; }
  if (!writeByte(reg))                         { i2cStop(); return false; }
  i2cStart();                                  // repeated start
  if (!writeByte((uint8_t)((addr << 1) | 1)))  { i2cStop(); return false; }
  value = readByte(false);                     // NACK the single byte
  i2cStop();
  return true;
}
static bool regWrite(uint8_t reg, uint8_t value){
  i2cStart();
  if (!writeByte((uint8_t)(addr << 1))) { i2cStop(); return false; }
  if (!writeByte(reg))                  { i2cStop(); return false; }
  if (!writeByte(value))                { i2cStop(); return false; }
  i2cStop();
  return true;
}

// ---- sensor -----------------------------------------------------------------
static int32_t cumX = 0, cumY = 0;
static uint32_t motionReads = 0;

static bool findSensor(){
  // Report the idle line levels first: both should read HIGH via the pull-ups.
  // A line stuck LOW is held down by something and no address will ever answer.
  pinMode(PIN_SDA, INPUT_PULLUP); pinMode(PIN_SCL, INPUT_PULLUP);
  delayMicroseconds(500);
  Serial.print("# idle lines: SDA="); Serial.print(digitalRead(PIN_SDA) ? "HIGH" : "LOW");
  Serial.print(" SCL=");              Serial.println(digitalRead(PIN_SCL) ? "HIGH" : "LOW");
  busBegin();

  for (uint8_t i = 0; i < sizeof(ADDRESSES); ++i){
    addr = ADDRESSES[i];
    uint8_t pid = 0xFF;
    bool ok = regRead(REG_PRODUCT_ID, pid);
    Serial.print("# probe 0x"); Serial.print(addr, HEX);
    Serial.print(" -> ");       Serial.print(ok ? "ACK pid=0x" : "no ACK (pid=0x");
    Serial.println(pid, HEX);
    if (ok && pid == PRODUCT_ID_EXPECTED){
      Serial.print("# found 0x31 at 0x"); Serial.println(addr, HEX);
      return true;
    }
  }
  addr = 0x00;
  Serial.println("# FAIL: nothing answered 0x31 on 0x75/0x73/0x79.");
  Serial.println("#   all 0xFF  -> no device driving: power, wiring, or missing pull-ups");
  Serial.println("#   all 0x00  -> SDA stuck low");
  Serial.println("#   If this is a TKMT (SPI) part, use the ../spi sketch instead.");
  return false;
}

static bool sensorInit(){
  if (!findSensor()) return false;
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
  uint8_t rx = 0, ry = 0;
  regRead(REG_RES_X, rx); regRead(REG_RES_Y, ry);
  Serial.print("# res_x=0x");  Serial.print(rx, HEX);
  Serial.print(" res_y=0x");   Serial.println(ry, HEX);
  return true;
}

static int16_t signExtend12(uint16_t v){       // 12-bit two's complement
  return (int16_t)((v & 0x800) ? (int16_t)(v | 0xF000) : (int16_t)v);
}

// Returns true if motion was present. Reads only when the motion bit is set --
// the chip accumulates between reads, so polling rate is not critical.
static bool readMotion(int16_t &dx, int16_t &dy){
  uint8_t motion = 0;
  if (!regRead(REG_MOTION, motion))   { dx = dy = 0; return false; }
  if (!(motion & MOTION_BIT))         { dx = dy = 0; return false; }
  uint8_t xl = 0, yl = 0, hi = 0;
  if (!regRead(REG_DELTA_X_LO, xl) ||
      !regRead(REG_DELTA_Y_LO, yl) ||
      !regRead(REG_DELTA_XY_H,  hi)) { dx = dy = 0; return false; }
  dx = signExtend12((uint16_t)(((uint16_t)(hi & 0xF0) << 4) | xl));
  dy = signExtend12((uint16_t)(((uint16_t)(hi & 0x0F) << 8) | yl));
  return true;
}

void setup(){
  Serial.begin(115200);
  while (!Serial && millis() < 3000) {}
  delay(50);                            // >= 10 ms power-on (DS Table 5)
  Serial.println("# pat9125 minimum reproducible case (I2C)");
  if (!sensorInit()){ Serial.println("# init failed -- halted"); while (1) delay(1000); }
  Serial.print("# counts_per_mm (nominal) = "); Serial.println(COUNTS_PER_MM, 2);
  Serial.println("# commands: z = zero, ? = status");
  Serial.println("ms,dx,dy,x_counts,y_counts,x_mm,y_mm");
}

void loop(){
  if (Serial.available()){
    char c = (char)Serial.read();
    if (c == 'z'){ cumX = cumY = 0; motionReads = 0; Serial.println("# zeroed"); }
    else if (c == '?'){
      uint8_t pid = 0xFF; regRead(REG_PRODUCT_ID, pid);
      Serial.print("# addr=0x");        Serial.print(addr, HEX);
      Serial.print(" pid=0x");          Serial.print(pid, HEX);
      Serial.print(" motion_reads=");   Serial.println(motionReads);
    }
  }

  int16_t dx, dy;
  if (readMotion(dx, dy)){ cumX += dx; cumY += dy; ++motionReads; }

  // Print at a fixed 50 Hz regardless of poll rate, so the log stays readable.
  static uint32_t lastPrint = 0;
  uint32_t now = millis();
  if (now - lastPrint >= 20){
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
