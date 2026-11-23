// =====================================================================
//  AUG-TEC MINI DOOM  -  a DOOM-style raycaster for the ESP32-C3 OLED mini
// =====================================================================
//  Board:   ESP32-C3 "OLED 0.42 inch" mini (the one with the tiny screen)
//  Screen:  0.96" ST7735S 80x160 color TFT plugged on the numbered row
//           (or the onboard 72x40 OLED when the TFT is unplugged)
//  Motion:  MPU-6500 on pins 10/9 (tilt = steer and walk)
//  Buttons: A on IO1, B on IO0
//
//  Why not REAL DOOM? Real DOOM needs about 4 MB of RAM. This chip has
//  about 400 KB and no extra PSRAM chip. So we use the same trick as
//  Wolfenstein 3D: a "raycaster". See HOW_IT_WORKS.md for the full story.
//
//  FILES IN THIS SKETCH:
//    doom_raycaster.ino  <- this file: pins, screens, tilt sensor, buttons
//    game_core.h         <- the game: map, monsters, 3D drawing (no pins!)
//
//  ARDUINO IDE SETTINGS (Tools menu):
//    Board: "ESP32C3 Dev Module"     USB CDC On Boot: "Enabled"
//    (CDC On Boot makes Serial print through the USB-C cable.)
// =====================================================================

#include <Arduino.h>
#include <Wire.h>              // I2C: talks to the MPU (and to probe the OLED)
#include <SPI.h>               // SPI: fast bus to the color TFT
#include <Adafruit_GFX.h>      // Adafruit's drawing base library
#include <Adafruit_ST7735.h>   // Driver for the ST7735S color screen
#include <U8g2lib.h>           // Driver for the onboard OLED
#include "game_core.h"         // Our game (map, monsters, raycaster)

// ---------------------------------------------------------------------
//  SETTINGS YOU MIGHT WANT TO CHANGE
// ---------------------------------------------------------------------
// Which screen to use:  0 = auto-detect,  1 = always TFT,  2 = always OLED
#define DISPLAY_MODE 0

// If the TFT shows inverted colors (black looks white) or is shifted by a
// couple of pixels with a garbage stripe, flip this between 0 and 1.
//   0 = INITR_MINI160x80          (offset 24, normal colors)
//   1 = INITR_MINI160x80_PLUGIN   (offset 26/1, inverted colors - common on IPS)
#define TFT_PLUGIN_VARIANT 0

// --- TFT COLOR FIXES (two switches, 0 or 1) ---
// Cheap ST7735S screens come in slightly different flavors, so the same
// colors can come out wrong. Compare your screen to the title photo:
//   Looks right (orange/yellow DOOM, brown/red brick rooms) -> leave them.
//   DOOM is DEEP BLUE and bricks are BLUE, black still black
//        -> red and blue are swapped: flip TFT_BGR.
//   DOOM is pale BLUEISH SILVER, rooms washed-out GREY/WHITE, dark parts
//   look light -> colors are inverted (like a photo negative): flip TFT_INVERT.
//   DOOM is orange but the background is WHITE and bricks are YELLOW
//        -> both are wrong: flip BOTH.
// TFT_INVERT: 1 = turn the screen's "invert" mode ON (most IPS panels need this)
#define TFT_INVERT 1
// TFT_BGR: 1 = the screen wants Blue-Green-Red order instead of Red-Green-Blue
#define TFT_BGR 0
// Turn the picture upside down if it's upside down: 1 or 3 (both landscape)
#define TFT_ROTATION 3
// SPI speed for the TFT. 27 MHz is safe for most. Try 40000000 for more fps.
#define TFT_SPI_HZ 8000000 

// --- Tilt axis setup ---
// The MPU measures tilt on 3 axes: X=0, Y=1, Z=2. We don't know yet how the
// MPU sits in the case, so pick which axis steers and which walks here.
// Open Serial Monitor (115200) - it prints the tilt numbers so you can see
// which one changes when you tilt left/right vs forward/back.
#define TURN_AXIS   1   // axis that changes when you tilt LEFT/RIGHT
#define TURN_INVERT 0   // set to 1 if turning goes the wrong way
#define WALK_AXIS   0   // axis that changes when you tilt FORWARD/BACK
#define WALK_INVERT 0   // set to 1 if walking goes the wrong way
// How much tilt does nothing (deadzone) and how much is "full speed".
// 1.0 = full gravity. 0.08 is about 5 degrees, 0.40 is about 24 degrees.
#define TILT_DEADZONE 0.08f
#define TILT_FULL     0.40f
// Print tilt values to Serial every second (1 = yes, 0 = no)
#define DEBUG_TILT 1

// AUTOPILOT (demo mode): the game walks and aims for you, you only shoot.
// 0 = only when no tilt sensor is found, 1 = always, 2 = never
#define AUTOPILOT 0

// ---------------------------------------------------------------------
//  PINS  (layout N: everything on the numbered row)
// ---------------------------------------------------------------------
// Color TFT (ST7735S, SPI)
#define PIN_TFT_SCK  8   // "SCL" on the screen = SPI clock. Board's blue LED is also on IO8, so it flickers. Normal!
#define PIN_TFT_MOSI 7   // "SDA" on the screen = SPI data
#define PIN_TFT_RST  6   // "RES" reset
#define PIN_TFT_DC   5   // "DC" data/command select
#define PIN_TFT_CS   4   // "CS" chip select
#define PIN_TFT_BLK  3   // "BLK" backlight (HIGH = on)
// MPU-6500 (I2C)
#define PIN_MPU_SDA  9   // note: IO9 is also the BOOT button. Don't press BOOT while playing.
#define PIN_MPU_SCL  10
// Onboard OLED (wired inside the board - source: github.com/selli106/ESP32-C3_OLED,
// oledanimationmaker.com ESP32-C3 0.42 OLED guide: SDA = GPIO5, SCL = GPIO6, addr 0x3C)
#define PIN_OLED_SDA 5
#define PIN_OLED_SCL 6
// Buttons: one leg to the pin, the other leg to GND
#define PIN_BTN_A    1   // A = shoot / start
#define PIN_BTN_B    0   // B = turn LEFT while held (doors open by themselves now)

// ---------------------------------------------------------------------
//  SCREEN OBJECTS
// ---------------------------------------------------------------------
// The TFT driver. &SPI = use the chip's hardware SPI (fast).
Adafruit_ST7735 tft(&SPI, PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST);
// The OLED driver. The ESP32-C3 has only ONE hardware I2C, and the MPU uses
// it on pins 9/10, so the OLED uses "software I2C" (the CPU wiggles pins 6/5).
// Constructor order: rotation, clock pin, data pin, reset pin.
U8G2_SSD1306_72X40_ER_F_SW_I2C oled(U8G2_R0, PIN_OLED_SCL, PIN_OLED_SDA, U8X8_PIN_NONE);

void present();         // "forward declaration": present() is written further down
bool useTFT = true;     // which screen we picked at boot
int screenW = 160;      // drawing size (160x80 TFT, or 72x40 OLED)
int screenH = 80;

// The framebuffer: one 16-bit color per pixel. 160*80*2 = 25,600 bytes.
// We draw the whole frame here first, then send it to the screen in one go.
// (Drawing straight onto the screen pixel by pixel would flicker and be slow.)
static uint16_t frame[160 * 80];
// For the OLED: 1 bit per pixel, packed 8 pixels per byte (72*40/8 = 360 bytes)
static uint8_t monoPages[72 * 5];

// ---------------------------------------------------------------------
//  BUTTONS (with debounce)
// ---------------------------------------------------------------------
// A real button "bounces": for a few milliseconds it flickers on/off as the
// metal contacts settle. Debouncing = only believe a change after it has
// stayed the same for DEBOUNCE_MS.
#define DEBOUNCE_MS 25

struct Button {
  uint8_t pin;
  bool down = false;          // the cleaned-up state: true = pressed
  bool lastRaw = false;       // what we read last time
  uint32_t rawChangedAt = 0;  // when the raw reading last changed
  uint32_t downSince = 0;     // when the press started
  bool justPressed = false;   // true for ONE loop after a press
  bool justReleased = false;  // true for ONE loop after a release
  uint32_t heldMs = 0;        // how long it was held (valid on release)

  void begin(uint8_t p) {
    pin = p;
    // INPUT_PULLUP: a resistor inside the chip pulls the pin up to 3.3V,
    // so it reads HIGH when not pressed. Pressing connects it to GND = LOW.
    pinMode(pin, INPUT_PULLUP);
  }

  void update(uint32_t now) {
    justPressed = justReleased = false;
    bool raw = (digitalRead(pin) == LOW);       // LOW = pressed (active low)
    if (raw != lastRaw) { lastRaw = raw; rawChangedAt = now; }
    if (raw != down && (now - rawChangedAt) >= DEBOUNCE_MS) {  // stable long enough
      down = raw;
      if (down) { justPressed = true; downSince = now; }
      else      { justReleased = true; heldMs = now - downSince; }
    }
  }
  uint32_t holdTime(uint32_t now) const { return down ? now - downSince : 0; }
};

Button btnA, btnB;

// ---------------------------------------------------------------------
//  TFT COLOR FIX: send two settings straight to the screen chip
// ---------------------------------------------------------------------
// We talk to the ST7735S directly (instead of trusting the library's
// defaults) so TFT_INVERT and TFT_BGR mean exactly the same thing no
// matter which TFT_PLUGIN_VARIANT you picked.
void applyTftColorFix() {
  // 1) Invert on/off. 0x21 = "INVON" (invert ON), 0x20 = "INVOFF".
  tft.sendCommand(TFT_INVERT ? ST77XX_INVON : ST77XX_INVOFF);

  // 2) MADCTL (command 0x36) = "which way do I draw, and in what color order?"
  //    It holds a few on/off bits:
  //      0x80 MY  = flip up/down       0x40 MX = flip left/right
  //      0x20 MV  = swap rows/columns (turns portrait into landscape)
  //      0x08 BGR = colors in Blue-Green-Red order instead of Red-Green-Blue
  //    These direction bits are the same ones the library uses for each rotation.
  static const uint8_t ROT_BITS[4] = {
    0x40 | 0x80,   // rotation 0: MX + MY
    0x80 | 0x20,   // rotation 1: MY + MV
    0x00,          // rotation 2: nothing
    0x40 | 0x20,   // rotation 3: MX + MV
  };
  uint8_t madctl = ROT_BITS[TFT_ROTATION & 3] | (TFT_BGR ? 0x08 : 0x00);
  tft.sendCommand(ST77XX_MADCTL, &madctl, 1);
  Serial.printf("[SCREEN] color fix: INVERT=%d BGR=%d (MADCTL=0x%02X)\n", TFT_INVERT, TFT_BGR, madctl);
}

// ---------------------------------------------------------------------
//  MPU-6500 TILT SENSOR (talking to its registers directly)
// ---------------------------------------------------------------------
// The MPU is a tiny chip with numbered "registers" (memory boxes).
// We write some to set it up, then read 6 bytes = 3 accelerometer numbers.
// The accelerometer feels GRAVITY. Lying flat, all gravity is on Z.
// Tilt it and some of gravity moves onto X or Y. That's how we sense tilt.
uint8_t mpuAddr = 0;            // 0x68 or 0x69 (found at boot), 0 = not found
float restX = 0, restY = 0, restZ = 0;   // reading when you held it still
float tiltF[3] = {0, 0, 0};              // smoothed tilt (gravity units)

void mpuWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(mpuAddr);
  Wire.write(reg);     // which register
  Wire.write(val);     // what to put in it
  Wire.endTransmission();
}

uint8_t mpuRead8(uint8_t addr, uint8_t reg) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return 0xFF;   // false = keep the bus for the read
  if (Wire.requestFrom(addr, (uint8_t)1) != 1) return 0xFF;
  return Wire.read();
}

bool mpuBegin() {
  // AD0 is cut on this build, so the address could be 0x68 or 0x69. Try both.
  const uint8_t candidates[2] = {0x68, 0x69};
  for (uint8_t a : candidates) {
    uint8_t who = mpuRead8(a, 0x75);          // 0x75 = WHO_AM_I register
    Serial.printf("[MPU] addr 0x%02X WHO_AM_I = 0x%02X\n", a, who);
    // 0x70 = MPU-6500, 0x68 = MPU-6050, 0x71/0x73 = MPU-9250/9255 clones...
    // Clones report all kinds of IDs, so accept ANY real answer.
    // 0xFF = nobody answered, 0x00 = bus stuck low. Anything else = a chip is there.
    if (who != 0xFF && who != 0x00) {
      mpuAddr = a;
      break;
    }
  }
  if (!mpuAddr) return false;
  mpuWrite(0x6B, 0x80); delay(100);   // PWR_MGMT_1: reset the chip
  mpuWrite(0x6B, 0x01); delay(10);    // wake up, use the accurate clock
  mpuWrite(0x1A, 0x03);               // CONFIG: gyro smoothing filter ~41 Hz
  mpuWrite(0x1B, 0x08);               // GYRO_CONFIG: +-500 deg/s (not used yet)
  mpuWrite(0x1C, 0x00);               // ACCEL_CONFIG: +-2g = most sensitive
  mpuWrite(0x1D, 0x03);               // ACCEL_CONFIG2: accel smoothing ~41 Hz
  return true;
}

// Read the 3 accelerometer values, converted to "g" (1.0 = gravity).
bool mpuReadAccel(float& x, float& y, float& z) {
  if (!mpuAddr) return false;
  Wire.beginTransmission(mpuAddr);
  Wire.write(0x3B);                   // ACCEL_XOUT_H: first of 6 bytes
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(mpuAddr, (uint8_t)6) != 6) return false;
  // Each value is 2 bytes: high byte first, then low byte. Glue them together.
  int16_t rx = (Wire.read() << 8) | Wire.read();
  int16_t ry = (Wire.read() << 8) | Wire.read();
  int16_t rz = (Wire.read() << 8) | Wire.read();
  // At +-2g, 16384 raw counts = 1 g.
  x = rx / 16384.0f; y = ry / 16384.0f; z = rz / 16384.0f;
  return true;
}

// Calibrate: hold still ~1 second, average the readings = your "neutral" pose.
// After this, tilt is measured RELATIVE to how you were holding it.
void calibrateTilt() {
  game::fillRect(0, 0, screenW, screenH, 0);
  game::drawTextCentered(screenH / 2 - 8, "HOLD STILL", game::rgb(255, 255, 255), 1);
  game::drawTextCentered(screenH / 2 + 2, "CALIBRATING", game::rgb(255, 200, 60), 1);
  present();                               // show it on the screen

  float sx = 0, sy = 0, sz = 0;
  int n = 0;
  for (int i = 0; i < 100; i++) {          // 100 samples x 10 ms = 1 second
    float x, y, z;
    if (mpuReadAccel(x, y, z)) { sx += x; sy += y; sz += z; n++; }
    delay(10);
  }
  if (n > 0) { restX = sx / n; restY = sy / n; restZ = sz / n; }
  tiltF[0] = tiltF[1] = tiltF[2] = 0;
  Serial.printf("[MPU] calibrated rest = %.2f %.2f %.2f (%d samples)\n", restX, restY, restZ, n);
}

// Turn a tilt amount into a control value from -1 to +1, with a deadzone
// so tiny hand wobbles don't make you drift.
float shapeTilt(float t) {
  float a = fabsf(t);
  if (a < TILT_DEADZONE) return 0;                       // inside deadzone: nothing
  float v = (a - TILT_DEADZONE) / (TILT_FULL - TILT_DEADZONE);
  if (v > 1) v = 1;                                      // cap at full speed
  return t < 0 ? -v : v;                                 // keep the direction
}

// ---------------------------------------------------------------------
//  SCREEN AUTO-DETECT
// ---------------------------------------------------------------------
// The onboard OLED is SOLDERED to the board, so it always answers on I2C,
// even with the TFT plugged on top. So "does the OLED answer?" can't tell
// us if the TFT is there. Instead we sense the TFT's BLK and CS pins:
// with nothing plugged in, those pins "float" and simply follow the chip's
// weak pull-up / pull-down resistor. A plugged-in screen module usually
// has its own resistor/transistor on them, so they stop following.
// (This is a heuristic! If it guesses wrong, hold a button at power-on:
//  hold A = force OLED, hold B = force TFT. Or set DISPLAY_MODE above.)
bool pinIsLoaded(uint8_t pin) {
  pinMode(pin, INPUT_PULLDOWN); delay(3);
  int withPullDown = digitalRead(pin);
  pinMode(pin, INPUT_PULLUP); delay(3);
  int withPullUp = digitalRead(pin);
  pinMode(pin, INPUT);
  // Floating pin: LOW with pull-down AND HIGH with pull-up. Anything else = something attached.
  return !(withPullDown == LOW && withPullUp == HIGH);
}

bool oledAnswers() {
  Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL, 100000);
  Wire.beginTransmission(0x3C);
  bool ack = (Wire.endTransmission() == 0);
  Wire.end();                                // free the I2C hardware for the MPU
  return ack;
}

bool detectTFT() {
  if (DISPLAY_MODE == 1) { Serial.println("[SCREEN] forced TFT by DISPLAY_MODE"); return true; }
  if (DISPLAY_MODE == 2) { Serial.println("[SCREEN] forced OLED by DISPLAY_MODE"); return false; }
  if (digitalRead(PIN_BTN_A) == LOW) { Serial.println("[SCREEN] A held at boot -> OLED"); return false; }
  if (digitalRead(PIN_BTN_B) == LOW) { Serial.println("[SCREEN] B held at boot -> TFT"); return true; }
  bool blk = pinIsLoaded(PIN_TFT_BLK);
  bool cs = pinIsLoaded(PIN_TFT_CS);
  bool oled = oledAnswers();
  Serial.printf("[SCREEN] BLK loaded=%d CS loaded=%d OLED ack=%d\n", blk, cs, oled);
  if (blk || cs) return true;   // something is plugged on the TFT pins
  if (!oled) return true;       // no OLED found either -> try TFT anyway
  return false;
}

// ---------------------------------------------------------------------
//  SEND THE FINISHED FRAME TO WHICHEVER SCREEN WE'RE USING
// ---------------------------------------------------------------------
void present() {
  if (useTFT) {
    // One big SPI transfer: set the window to the whole screen and blast
    // all 12,800 pixels. The ESP32 SPI driver swaps the byte order for us.
    tft.startWrite();
    tft.setAddrWindow(0, 0, screenW, screenH);
    tft.writePixels(frame, (uint32_t)screenW * screenH);
    tft.endWrite();
  } else {
    // Color -> black & white dots, then copy straight into U8g2's buffer.
    game::ditherToPages(monoPages);
    memcpy(oled.getBufferPtr(), monoPages, sizeof(monoPages));
    oled.sendBuffer();
  }
}

// ---------------------------------------------------------------------
//  SETUP: runs once at power-on
// ---------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(300);                                  // give USB a moment
  Serial.println("\n=== AUG-TEC MINI DOOM ===");

  btnA.begin(PIN_BTN_A);
  btnB.begin(PIN_BTN_B);
  delay(5);

  // 1) Which screen?
  useTFT = detectTFT();
  if (useTFT) {
    screenW = 160; screenH = 80;
    pinMode(PIN_TFT_BLK, OUTPUT);
    digitalWrite(PIN_TFT_BLK, HIGH);           // backlight on
    SPI.begin(PIN_TFT_SCK, -1, PIN_TFT_MOSI, PIN_TFT_CS);  // -1 = no MISO (screen never talks back)
#if TFT_PLUGIN_VARIANT
    tft.initR(INITR_MINI160x80_PLUGIN);
#else
    tft.initR(INITR_MINI160x80);
#endif
    tft.setSPISpeed(TFT_SPI_HZ);
    tft.setRotation(TFT_ROTATION);             // landscape: 160 wide, 80 tall
    applyTftColorFix();                        // invert + RGB/BGR from the settings at the top
    tft.fillScreen(ST77XX_BLACK);
    Serial.println("[SCREEN] using color TFT 160x80");
  } else {
    screenW = 72; screenH = 40;
    oled.begin();
    oled.setContrast(255);
    Serial.println("[SCREEN] using onboard OLED 72x40");
  }

  // 2) Start the game (builds textures, loads the map).
  game::init(frame, screenW, screenH);

  // 3) Tilt sensor on the hardware I2C (pins 9 = SDA, 10 = SCL).
  // Try the MPU on 9/10 first. If nobody answers, try the wires the other
  // way around (SDA<->SCL swapped) - a super common mix-up.
  bool mpuOk = false;
  for (int attempt = 0; attempt < 2 && !mpuOk; attempt++) {
    int sda = attempt == 0 ? PIN_MPU_SDA : PIN_MPU_SCL;
    int scl = attempt == 0 ? PIN_MPU_SCL : PIN_MPU_SDA;
    Wire.end();
    Wire.begin(sda, scl, 100000);              // 100 kHz = slow and safe for no-solder wires
    Wire.setTimeOut(20);                       // don't hang on a dead bus
    Serial.printf("[I2C] scan SDA=%d SCL=%d (line levels SDA=%d SCL=%d):",
                  sda, scl, digitalRead(sda), digitalRead(scl));
    int found = 0;
    for (uint8_t a = 1; a < 127; a++) {
      Wire.beginTransmission(a);
      if (Wire.endTransmission() == 0) { Serial.printf(" 0x%02X", a); found++; }
    }
    Serial.printf("  (%d found)\n", found);
    Serial.flush();
    mpuOk = mpuBegin();
    if (mpuOk && attempt == 1) Serial.println("[MPU] works with SDA/SCL SWAPPED - your wires are crossed (that's fine, code handles it)");
  }
  if (mpuOk) {
    Serial.printf("[MPU] found at 0x%02X\n", mpuAddr);
    calibrateTilt();
  } else {
    Serial.println("[MPU] NOT FOUND - check wiring (VCC 3V3, GND, SDA=9, SCL=10)");
    game::showMessage("NO MPU");
  }
}

// ---------------------------------------------------------------------
//  LOOP: the game loop. Every pass = one frame:
//        read inputs -> update the game -> draw -> show
// ---------------------------------------------------------------------
uint32_t lastMs = 0, fpsTimer = 0;
int frames = 0;

void loop() {
  uint32_t now = millis();
  int dt = (int)(now - lastMs);               // time since last frame
  if (dt > 50) dt = 50;                       // don't jump too far after a hiccup
  lastMs = now;

  // ---- 1. Read the buttons ----
  btnA.update(now);
  btnB.update(now);

  // Hold A AND B together for 1 second = recalibrate tilt (once per hold).
  // (It used to be "hold B", but holding B now means "turn left".)
  static bool recalDone = false;
  if (btnA.holdTime(now) > 1000 && btnB.holdTime(now) > 1000 && !recalDone && mpuAddr) {
    recalDone = true;
    calibrateTilt();
    game::showMessage("RECALIBRATED");
    lastMs = millis();
    return;
  }
  // A short tap of B still counts as "use" (starts the game on the title
  // screen, restarts after you die). Doors open by themselves while playing.
  bool bTap = btnB.justReleased && btnB.heldMs < 600 && !recalDone;
  if (btnA.justReleased || btnB.justReleased) recalDone = false;

  // ---- 2. Read the tilt sensor ----
  float ax, ay, az;
  if (mpuReadAccel(ax, ay, az)) {
    float raw[3] = {ax - restX, ay - restY, az - restZ};   // change from neutral
    for (int i = 0; i < 3; i++) tiltF[i] += (raw[i] - tiltF[i]) * 0.35f;  // smooth out jitter
  }
  float turn = shapeTilt(tiltF[TURN_AXIS]) * (TURN_INVERT ? -1 : 1);
  float walk = shapeTilt(tiltF[WALK_AXIS]) * (WALK_INVERT ? -1 : 1);

  // ---- 3. Hand the controls to the game and let time pass ----
  game::Input in;
  in.turn = turn;
  in.move = walk;
  in.fireHeld = btnA.down;
  in.firePressed = btnA.justPressed;
  in.usePressed = bTap;
  // No tilt sensor (or AUTOPILOT forced on)? Let the game walk and aim for you.
  if ((AUTOPILOT == 1) || (AUTOPILOT == 0 && !mpuAddr)) game::autopilot(in, dt);
  // Holding B = turn LEFT at full turn speed (-1 = full left). This comes
  // AFTER the autopilot, so B wins while you hold it; let go and the
  // autopilot takes over steering again.
  if (btnB.down && !recalDone) in.turn = -1.0f;
  game::update(in, dt);

  // ---- 4. Draw the frame into memory, then push it to the screen ----
  game::render();
  present();

  // ---- 5. Debug info once per second ----
  frames++;
  if (now - fpsTimer >= 1000) {
    Serial.printf("fps=%d", frames);
#if DEBUG_TILT
    Serial.printf("  tilt X=%+.2f Y=%+.2f Z=%+.2f  turn=%+.2f walk=%+.2f",
                  tiltF[0], tiltF[1], tiltF[2], turn, walk);
#endif
    Serial.printf("  hp=%d ammo=%d kills=%d\n", game::player.hp, game::player.ammo, game::player.kills);
    frames = 0;
    fpsTimer = now;
  }
}
