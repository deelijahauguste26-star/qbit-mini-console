// =====================================================================
//  game_core.h  -  THE GAME ITSELF (map, monsters, drawing the 3D view)
// =====================================================================
//  This file has NO Arduino code in it on purpose. It only does math and
//  writes colors into a plain array of pixels (a "framebuffer").
//  Why? Two reasons:
//   1) The same game can draw to the color TFT (160x80) OR the tiny
//      onboard OLED (72x40). The .ino file decides where the pixels go.
//   2) We can also compile this exact file on a normal PC to make preview
//      pictures (see ../sim/). Same code = honest preview.
//
//  How a "raycaster" works (the trick Wolfenstein 3D used in 1992):
//   - The world is a flat grid of squares, like graph paper (the MAP below).
//     Each square is either empty floor or a wall block.
//   - For every column of pixels on the screen we shoot one invisible
//     "ray" from the player into the grid until it hits a wall.
//   - Far wall = short stripe, close wall = tall stripe. Do that for every
//     column and your brain sees a 3D room. No real 3D needed!
//   - Monsters and items are flat pictures ("sprites") that always face
//     you, drawn after the walls, hidden behind walls that are closer.
//
//  Speed note: the ESP32-C3 has no floating-point hardware (no "FPU"), so
//  decimal math with float is slow-ish. The hot loop (once per pixel
//  column, many times per frame) uses FIXED-POINT math: we store numbers
//  as whole integers that are secretly multiplied by 65536. So 1.5 is
//  stored as 98304. Integer math is fast on the C3. The slow-but-easy
//  float math is only used a handful of times per frame (game logic).
// =====================================================================
#pragma once
#include <stdint.h>
#include <math.h>
#include <string.h>
#include <stdio.h>

namespace game {

// ---------------------------------------------------------------------
//  Little helpers
// ---------------------------------------------------------------------

// Pack red, green, blue (each 0-255) into one 16-bit "RGB565" color.
// RGB565 = 5 bits red, 6 bits green, 5 bits blue. That's what the TFT eats.
static inline uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// Make a color darker. shade256 = 256 means "full bright", 128 = half.
static inline uint16_t shade(uint16_t c, int shade256) {
  int r = (c >> 11) & 0x1F;          // pull the 5 red bits out
  int g = (c >> 5) & 0x3F;           // pull the 6 green bits out
  int b = c & 0x1F;                  // pull the 5 blue bits out
  r = (r * shade256) >> 8;           // scale each one down
  g = (g * shade256) >> 8;
  b = (b * shade256) >> 8;
  return (uint16_t)((r << 11) | (g << 5) | b);  // pack back together
}

// Mix a color toward red (used when you get hurt).
static inline uint16_t tintRed(uint16_t c, int amount256) {
  int r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
  r = r + (((31 - r) * amount256) >> 8);  // push red up
  g = (g * (256 - amount256)) >> 8;       // pull green down
  b = (b * (256 - amount256)) >> 8;       // pull blue down
  return (uint16_t)((r << 11) | (g << 5) | b);
}

// Tiny random number maker (so we don't need Arduino's random()).
static uint32_t rngState = 12345;
static inline uint32_t rnd() {
  rngState = rngState * 1103515245u + 12345u;  // classic "LCG" formula
  return (rngState >> 16) & 0x7FFF;
}

// One special color means "see-through" inside sprite pictures.
static const uint16_t TRANSPARENT = 0xF81F;  // bright magenta, never used for real

// ---------------------------------------------------------------------
//  Tunable game settings (change these to make it easier/harder)
// ---------------------------------------------------------------------
static const float MOVE_SPEED     = 2.2f;   // map squares per second at full tilt
static const float TURN_SPEED     = 2.6f;   // radians per second at full tilt (~150 deg/s)
static const float PLAYER_RADIUS  = 0.22f;  // how "fat" you are for wall bumping
static const float ENEMY_SPEED    = 0.9f;   // monster walking speed
static const float ENEMY_SIGHT    = 9.0f;   // monsters wake up inside this distance
static const int   START_HP       = 100;
static const int   START_AMMO     = 30;
static const int   FIRE_DELAY_MS  = 260;    // time between shots when holding A
static const float FOV_PLANE      = 0.66f;  // field of view (0.66 = about 66 degrees)

// ---------------------------------------------------------------------
//  THE MAP  (16 x 16 squares)
// ---------------------------------------------------------------------
//  Each character is one square:
//   '.' = empty floor          '1' = gray stone wall
//   '2' = red brick wall       '3' = green tech wall
//   '4' = blue metal wall      'D' = door (opens by itself when you walk up to it)
//   'P' = where you start      'E' = monster
//   'H' = health pack          'A' = ammo box
//  Edit this drawing to make your own level! Keep the outside border walls.
static const int MAP_W = 16;
static const int MAP_H = 16;
static const char* const LEVEL[MAP_H] = {
  "1111111111111111",
  "1P.....1.....E.1",
  "1......1.......1",
  "1..22..D...33..1",
  "1..22..1...33..1",
  "1......1.......1",
  "1111D111111D1111",
  "1......4.......1",
  "1.E....4...E...1",
  "1......D.......1",
  "1..H...4....A..1",
  "1444D444...2...1",
  "1......4...2.E.1",
  "1.E..A.D...2...1",
  "1......4.H.....1",
  "1111111111111111",
};

// The live map (a copy we can change, e.g. when doors open).
// 0 = empty, 1-4 = wall types, 9 = door.
static uint8_t worldMap[MAP_H][MAP_W];

// Is this square solid? (outside the map counts as solid, so you can't escape)
static inline bool isWall(int mx, int my) {
  if (mx < 0 || my < 0 || mx >= MAP_W || my >= MAP_H) return true;
  return worldMap[my][mx] != 0;
}

// ---------------------------------------------------------------------
//  WALL TEXTURES  (little 16x16 pictures painted on the walls)
// ---------------------------------------------------------------------
//  Instead of storing image files, we "paint" them with code at startup.
//  5 textures x 16 x 16 pixels x 2 bytes = 2.5 KB of RAM. Cheap.
static const int TEX = 16;
static uint16_t textures[5][TEX * TEX];

// Which texture goes with which map number.
static inline int texFor(uint8_t tile) {
  switch (tile) {
    case 1: return 0;  // stone
    case 2: return 1;  // brick
    case 3: return 2;  // tech
    case 4: return 3;  // metal
    default: return 4; // door
  }
}

static void buildTextures() {
  for (int y = 0; y < TEX; y++) {
    for (int x = 0; x < TEX; x++) {
      int n = (int)(rnd() % 24) - 12;            // a bit of random "grain"

      // 0) Gray stone blocks: big blocks with dark gaps (mortar)
      bool mortarS = (y % 8 == 7) || (((y / 8) % 2 == 0) ? (x == 15) : (x == 7));
      int g = 120 + n;
      textures[0][y * TEX + x] = mortarS ? rgb(50, 50, 55) : rgb(g, g, g + 8);

      // 1) Red bricks: small bricks, every other row shifted half a brick
      bool mortarB = (y % 4 == 3) || ((((y / 4) % 2) ? (x + 4) : x) % 8 == 7);
      textures[1][y * TEX + x] = mortarB ? rgb(90, 80, 70) : rgb(150 + n, 45 + n / 2, 35);

      // 2) Green tech panel: frame, a slot and two glowing lights
      uint16_t c = rgb(40, 110 + n, 60);
      if (x == 0 || y == 0 || x == 15 || y == 15) c = rgb(20, 50, 30);  // frame
      if (y >= 6 && y <= 9 && x >= 3 && x <= 12) c = rgb(15, 35, 20);   // slot
      if ((x == 4 || x == 11) && (y == 3)) c = rgb(255, 240, 80);        // lights
      textures[2][y * TEX + x] = c;

      // 3) Blue metal: plates with rivets in the corners
      c = rgb(50, 70, 140 + n);
      if (y == 0 || y == 8) c = rgb(25, 35, 80);                       // seams
      if ((x == 2 || x == 13) && (y == 2 || y == 6 || y == 10 || y == 14)) c = rgb(200, 210, 240);
      textures[3][y * TEX + x] = c;

      // 4) Door: wooden planks with a metal band and a handle
      c = rgb(120 + n, 75 + n / 2, 35);
      if (x % 4 == 0) c = rgb(70, 40, 20);                               // plank gaps
      if (y == 3 || y == 12) c = rgb(110, 110, 120);                     // metal bands
      if (x >= 11 && x <= 12 && y >= 7 && y <= 8) c = rgb(230, 200, 60); // handle
      textures[4][y * TEX + x] = c;
    }
  }
}

// ---------------------------------------------------------------------
//  SPRITE PICTURES  (monsters, items, your gun) drawn as text art
// ---------------------------------------------------------------------
//  Each letter = one pixel color (see colorOf() below). '.' = see-through.
static const char* const IMP_ART[16] = {   // the monster
  "......kkkk......",
  ".....kRRRRk.....",
  "....kRyRRyRk....",
  "....kRRRRRRk....",
  "....kRwwwwRk....",
  ".....kRRRRk.....",
  "...kkRRRRRRkk...",
  "..kRRkRRRRkRRk..",
  "..kRk.kRRk.kRk..",
  "..kRk.kRRk.kRk..",
  "..yy..kRRk..yy..",
  "......kRRk......",
  ".....kRk.kRk....",
  ".....kRk.kRk....",
  "....kRRk.kRRk...",
  "....kkk...kkk...",
};
static const char* const CORPSE_ART[16] = { // what's left after you win the fight
  "................", "................", "................", "................",
  "................", "................", "................", "................",
  "................", "................", "................", "................",
  "................",
  "....kkk..kk.....",
  "..kRRrRkRRrRk...",
  ".rrRRRRRRRRRrr..",
};
static const char* const HEALTH_ART[16] = { // white box, red cross
  "................", "................", "................", "................",
  "................", "................",
  "...kkkkkkkkkk...",
  "...kwwwwwwwwk...",
  "...kwwwrrwwwk...",
  "...kwwwrrwwwk...",
  "...kwrrrrrrwk...",
  "...kwrrrrrrwk...",
  "...kwwwrrwwwk...",
  "...kwwwrrwwwk...",
  "...kwwwwwwwwk...",
  "...kkkkkkkkkk...",
};
static const char* const AMMO_ART[16] = {   // green crate with bullets on top
  "................", "................", "................", "................",
  "................", "................", "................",
  ".....y.y.y......",
  ".....y.y.y......",
  "...kkkkkkkkkk...",
  "...kGGGGGGGGk...",
  "...kGkkkkkkGk...",
  "...kGGGGGGGGk...",
  "...kGGGGGGGGk...",
  "...kGGGGGGGGk...",
  "...kkkkkkkkkk...",
};
static const char* const GUN_ART[12] = {    // your pistol + hand, seen from behind
  "......kkkk......",
  "......kggk......",
  "......kWgk......",
  "......kggk......",
  ".....kggggk.....",
  ".....kggggk.....",
  "....kkggggkk....",
  "...kssggggssk...",
  "..ksssskkssssk..",
  "..kssssssssssk..",
  ".ksssssssssssk..",
  ".kssssssssssssk.",
};
static const char* const FLASH_ART[8] = {   // muzzle flash when you shoot
  "...y..y..y..",
  "....yyyyy...",
  ".y.yWWWWWy.y",
  "..yWWWWWWWy.",
  ".yyWWWWWWWyy",
  "..yWWWWWWWy.",
  ".y.yyWWWyy.y",
  "....y.y.y...",
};

// Turn an art letter into an actual color.
static uint16_t colorOf(char ch) {
  switch (ch) {
    case 'k': return rgb(25, 10, 10);     // dark outline
    case 'R': return rgb(170, 70, 40);    // monster skin
    case 'r': return rgb(200, 20, 20);    // red
    case 'y': return rgb(255, 220, 60);   // yellow
    case 'w': return rgb(240, 240, 240);  // white
    case 'W': return rgb(255, 255, 210);  // hot white
    case 'g': return rgb(90, 95, 105);    // gun metal
    case 's': return rgb(220, 160, 120);  // skin (your hand)
    case 'G': return rgb(60, 120, 50);    // crate green
    default:  return TRANSPARENT;         // '.' and anything else
  }
}

// ---------------------------------------------------------------------
//  TINY FONT  (3 pixels wide, 5 tall) so text works on BOTH screens
// ---------------------------------------------------------------------
//  Each glyph is 15 "bits" written as 1s and 0s: 5 rows of 3.
struct Glyph { char c; const char* bits; };
static const Glyph FONT[] = {
  {'0',"111101101101111"},{'1',"010110010010111"},{'2',"111001111100111"},
  {'3',"111001111001111"},{'4',"101101111001001"},{'5',"111100111001111"},
  {'6',"111100111101111"},{'7',"111001010010010"},{'8',"111101111101111"},
  {'9',"111101111001111"},{'A',"010101111101101"},{'B',"110101110101110"},
  {'C',"011100100100011"},{'D',"110101101101110"},{'E',"111100110100111"},
  {'F',"111100110100100"},{'G',"011100101101011"},{'H',"101101111101101"},
  {'I',"111010010010111"},{'J',"001001001101010"},{'K',"101101110101101"},
  {'L',"100100100100111"},{'M',"101111111101101"},{'N',"110101101101101"},
  {'O',"010101101101010"},{'P',"110101110100100"},{'Q',"010101101110011"},
  {'R',"110101110101101"},{'S',"011100010001110"},{'T',"111010010010010"},
  {'U',"101101101101111"},{'V',"101101101101010"},{'W',"101101111111101"},
  {'X',"101101010101101"},{'Y',"101101010010010"},{'Z',"111001010100111"},
  {'-',"000000111000000"},{'/',"001001010100100"},{':',"000010000010000"},
  {'!',"010010010000010"},{'+',"000010111010000"},{'.',"000000000000010"},{'=',"000111000111000"},
};

static const char* glyphBits(char c) {
  if (c >= 'a' && c <= 'z') c = (char)(c - 32);  // lowercase -> uppercase
  for (unsigned i = 0; i < sizeof(FONT) / sizeof(FONT[0]); i++)
    if (FONT[i].c == c) return FONT[i].bits;
  return nullptr;  // unknown character (or space) = blank
}

// Width in pixels of a piece of text at a given size.
static int textWidth(const char* s, int scale) {
  return (int)strlen(s) * 4 * scale - scale;  // 3 px glyph + 1 px gap
}

// ---------------------------------------------------------------------
//  THE FRAMEBUFFER  (where we draw)
// ---------------------------------------------------------------------
//  A frame is just W*H colors in a row, left-to-right, top-to-bottom.
//  pixel (x,y) lives at fb[y * W + x].
static uint16_t* fb = nullptr;
static int W = 160, H = 80;

static inline void putPixel(int x, int y, uint16_t c) {
  if (x < 0 || y < 0 || x >= W || y >= H) return;  // ignore off-screen
  fb[y * W + x] = c;
}

static void fillRect(int x, int y, int w, int h, uint16_t c) {
  for (int j = y; j < y + h; j++)
    for (int i = x; i < x + w; i++) putPixel(i, j, c);
}

// Draw text. shadow=true draws a black copy 1px down-right so it's readable.
static void drawText(int x, int y, const char* s, uint16_t color, int scale, bool shadow = true) {
  if (shadow) drawText(x + 1, y + 1, s, 0x0000, scale, false);
  for (; *s; s++, x += 4 * scale) {
    const char* bits = glyphBits(*s);
    if (!bits) continue;
    for (int row = 0; row < 5; row++)
      for (int col = 0; col < 3; col++)
        if (bits[row * 3 + col] == '1')
          fillRect(x + col * scale, y + row * scale, scale, scale, color);
  }
}

static void drawTextCentered(int y, const char* s, uint16_t color, int scale) {
  drawText((W - textWidth(s, scale)) / 2, y, s, color, scale);
}

// Draw a text-art picture (like GUN_ART) scaled up by 'scale', top-left at x,y.
static void drawArt(const char* const* art, int rows, int x, int y, int scale) {
  for (int r = 0; r < rows; r++) {
    const char* line = art[r];
    for (int cIdx = 0; line[cIdx]; cIdx++) {
      uint16_t col = colorOf(line[cIdx]);
      if (col == TRANSPARENT) continue;
      fillRect(x + cIdx * scale, y + r * scale, scale, scale, col);
    }
  }
}

// ---------------------------------------------------------------------
//  GAME STATE  (everything that changes while you play)
// ---------------------------------------------------------------------
enum State { TITLE, PLAY, DEAD, WIN };
enum ThingType { ENEMY, CORPSE, HEALTH, AMMO, USED };

struct Thing {          // a monster or an item lying on the floor
  float x, y;           // where it is on the map
  uint8_t type;         // ENEMY, HEALTH, ...
  int8_t hp;            // monster health
  bool awake;           // has the monster noticed you?
  int16_t hurtMs;       // >0 = flashing white because you hit it
  int16_t attackMs;     // countdown until it can claw you again
  float dist;           // distance to you (for drawing far-to-near)
};

static const int MAX_THINGS = 24;
static Thing things[MAX_THINGS];
static int thingCount = 0;

struct Player { float x, y, ang; int hp, ammo, kills; };
static Player player;

static State state = TITLE;
static int totalEnemies = 0;
static int flashMs = 0;        // muzzle flash timer
static int fireCooldown = 0;   // time until you can shoot again
static int hurtMs = 0;         // red screen timer
static int stateMs = 0;        // how long we've been in this state
static float bobPhase = 0;     // gun bobbing while walking
static char message[20] = "";  // short popup text ("DOOR OPEN" etc.)
static int messageMs = 0;
static float titleAng = 0;     // camera spin on the title screen

// Depth of the wall in each screen column (so sprites hide behind walls).
// Size 160 = biggest screen width we support.
static int32_t zbuf[160];

static void showMessage(const char* m) {
  strncpy(message, m, sizeof(message) - 1);
  message[sizeof(message) - 1] = 0;
  messageMs = 1400;
}

// Set up a brand new game: copy the map, place monsters and items.
static void newGame() {
  thingCount = 0;
  totalEnemies = 0;
  for (int y = 0; y < MAP_H; y++) {
    for (int x = 0; x < MAP_W; x++) {
      char ch = LEVEL[y][x];
      worldMap[y][x] = 0;
      float cx = x + 0.5f, cy = y + 0.5f;  // center of the square
      if (ch >= '1' && ch <= '4') worldMap[y][x] = (uint8_t)(ch - '0');
      else if (ch == 'D') worldMap[y][x] = 9;
      else if (ch == 'P') { player.x = cx; player.y = cy; player.ang = 0; }
      else if ((ch == 'E' || ch == 'H' || ch == 'A') && thingCount < MAX_THINGS) {
        Thing& t = things[thingCount++];
        memset(&t, 0, sizeof(t));
        t.x = cx; t.y = cy;
        t.type = (ch == 'E') ? ENEMY : (ch == 'H') ? HEALTH : AMMO;
        t.hp = 2;                             // 2 shots per monster
        t.attackMs = 600;
        if (ch == 'E') totalEnemies++;
      }
    }
  }
  player.hp = START_HP;
  player.ammo = START_AMMO;
  player.kills = 0;
  flashMs = fireCooldown = hurtMs = messageMs = 0;
}

// Called once at power-on.
static void init(uint16_t* framebuffer, int width, int height) {
  fb = framebuffer;
  W = width;
  H = height;
  buildTextures();
  newGame();
  state = TITLE;
}

// ---------------------------------------------------------------------
//  MOVEMENT + COLLISION
// ---------------------------------------------------------------------
// Would a circle of radius r centered at (x,y) touch any wall?
static bool blocked(float x, float y, float r) {
  return isWall((int)(x - r), (int)(y - r)) || isWall((int)(x + r), (int)(y - r)) ||
         isWall((int)(x - r), (int)(y + r)) || isWall((int)(x + r), (int)(y + r));
}

// Try to move. We test X and Y separately so you SLIDE along walls
// instead of sticking to them.
static void tryMove(float& x, float& y, float dx, float dy, float r) {
  if (!blocked(x + dx, y, r)) x += dx;
  if (!blocked(x, y + dy, r)) y += dy;
}

// Can point A see point B? Walk along the line in small steps checking walls.
static bool lineOfSight(float ax, float ay, float bx, float by) {
  float dx = bx - ax, dy = by - ay;
  float len = sqrtf(dx * dx + dy * dy);
  int steps = (int)(len * 8) + 1;   // 8 checks per square
  for (int i = 1; i < steps; i++) {
    float t = (float)i / steps;
    if (isWall((int)(ax + dx * t), (int)(ay + dy * t))) return false;
  }
  return true;
}

// ---------------------------------------------------------------------
//  INPUT  (filled in by the .ino from the tilt sensor and buttons)
// ---------------------------------------------------------------------
struct Input {
  float turn;        // -1 (full left) .. 0 .. +1 (full right)
  float move;        // -1 (full back) .. 0 .. +1 (full forward)
  bool fireHeld;     // button A is down right now
  bool firePressed;  // button A was JUST pressed this frame
  bool usePressed;   // button B was tapped (short press): starts/restarts the game
};

// Shoot! Find the closest monster near the middle of the screen.
static void shoot() {
  float c = cosf(player.ang), s = sinf(player.ang);
  int best = -1;
  float bestDepth = 1e9f;
  float wallDepth = zbuf[W / 2] / 65536.0f;          // wall straight ahead
  for (int i = 0; i < thingCount; i++) {
    Thing& t = things[i];
    if (t.type != ENEMY) continue;
    float dx = t.x - player.x, dy = t.y - player.y;
    float depth = dx * c + dy * s;                   // how far in front of you
    float side = -dx * s + dy * c;                   // how far left/right of center
    if (depth < 0.2f || depth > wallDepth) continue; // behind you, or behind a wall
    if (fabsf(side) > 0.30f) continue;               // not lined up with the crosshair
    if (depth < bestDepth) { bestDepth = depth; best = i; }
  }
  if (best >= 0) {
    Thing& t = things[best];
    t.hp--;
    t.hurtMs = 150;
    t.awake = true;
    if (t.hp <= 0) {
      t.type = CORPSE;
      player.kills++;
    }
  }
}

// Open a door right in front of you (looks 0.5 to 1.3 squares ahead).
// Called every frame, so doors open automatically as you walk up to them.
static void useAction() {
  float c = cosf(player.ang), s = sinf(player.ang);
  for (float reach = 0.5f; reach <= 1.3f; reach += 0.2f) {
    int mx = (int)(player.x + c * reach), my = (int)(player.y + s * reach);
    if (mx < 0 || my < 0 || mx >= MAP_W || my >= MAP_H) return;
    if (worldMap[my][mx] == 9) { worldMap[my][mx] = 0; showMessage("DOOR OPEN"); return; }
    if (worldMap[my][mx] != 0) return;  // hit a normal wall first
  }
}

// ---------------------------------------------------------------------
//  AUTOPILOT (demo mode): the game walks and aims FOR you, you just shoot.
//  Used when there's no tilt sensor, so you can still show it off.
// ---------------------------------------------------------------------
// Keep an angle difference between -pi and +pi (so we turn the short way).
static float wrapAngle(float a) {
  while (a > 3.14159265f) a -= 6.2831853f;
  while (a < -3.14159265f) a += 6.2831853f;
  return a;
}

// ---- Autopilot memory ----
// visited[y][x] = 1 once we've stood in that square. The autopilot always
// heads for the NEAREST square it hasn't been to yet, so it explores the
// whole map (and goes through doors to get there) instead of wandering.
static uint8_t autoVisited[MAP_H][MAP_W];
static int16_t autoCameFrom[MAP_H * MAP_W];   // for the path search below
static int16_t autoQueue[MAP_H * MAP_W];
static int autoStuckMs = 0;       // how long we've tried to walk without moving
static int autoBackupMs = 0;      // >0 = backing up to get unstuck
static int autoBackupDir = 1;     // which way to turn while backing up
static float autoLastX = 0, autoLastY = 0;
static int autoGoal = -1;         // the square we're heading for (we stick to it, so we don't dither)

// Squares we can path through: empty floor, or a door (we'll open it).
static inline bool autoWalkable(int x, int y) {
  if (x < 0 || y < 0 || x >= MAP_W || y >= MAP_H) return false;
  return worldMap[y][x] == 0 || worldMap[y][x] == 9;
}

// "Breadth-first search": spread out from our square one step at a time
// (like a ripple in water) until we reach a goal square. Then walk the
// ripple backwards to find the FIRST step of the shortest path.
// goal: 0 = nearest unvisited square, 1 = the square of a live monster,
//       2 = exactly the square autoGoal.
// Returns the index (y*16+x) of the next square to step into, or -1.
// The goal square it found is saved in autoGoal.
static int autoNextStep(int goal) {
  int sx = (int)player.x, sy = (int)player.y, start = sy * MAP_W + sx;
  for (int i = 0; i < MAP_W * MAP_H; i++) autoCameFrom[i] = -1;
  int head = 0, tail = 0;
  autoQueue[tail++] = (int16_t)start; autoCameFrom[start] = (int16_t)start;
  while (head < tail) {
    int cur = autoQueue[head++], cx = cur % MAP_W, cy = cur / MAP_W;
    bool isGoal = false;
    if (cur != start) {
      if (goal == 0) isGoal = !autoVisited[cy][cx];
      else if (goal == 2) isGoal = (cur == autoGoal);
      else for (int i = 0; i < thingCount; i++)
        if (things[i].type == ENEMY && (int)things[i].x == cx && (int)things[i].y == cy) isGoal = true;
    }
    if (isGoal) {
      autoGoal = cur;
      while (autoCameFrom[cur] != start) cur = autoCameFrom[cur];  // walk back to step 1
      return cur;
    }
    if (worldMap[cy][cx] == 9) continue;   // a closed door: we can reach it, not go past it (yet)
    const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};   // right, left, down, up (no diagonals = no corner snags)
    for (int k = 0; k < 4; k++) {
      int nx = cx + dx[k], ny = cy + dy[k], n = ny * MAP_W + nx;
      if (autoWalkable(nx, ny) && autoCameFrom[n] < 0) { autoCameFrom[n] = (int16_t)cur; autoQueue[tail++] = (int16_t)n; }
    }
  }
  return -1;
}

// Turn toward point (tx, ty). Only walk once we're roughly facing it, so we
// go straight through doorways instead of scraping along the frame.
static void autoSteerTo(Input& in, float tx, float ty, float speed) {
  float diff = wrapAngle(atan2f(ty - player.y, tx - player.x) - player.ang);
  in.turn = constrain(diff * 3.0f, -1.0f, 1.0f);
  float a = fabsf(diff);
  in.move = a < 0.35f ? speed : (a < 0.9f ? speed * 0.3f : 0.0f);
}

// Fills in turn/move/use for you. You still press A to shoot.
static void autopilot(Input& in, int dtMs) {
  in.turn = 0; in.move = 0;
  if (state != PLAY) return;
  if (stateMs == 0) {                               // a new game just started: forget everything
    memset(autoVisited, 0, sizeof(autoVisited));
    autoStuckMs = autoBackupMs = 0;
    autoGoal = -1;
    autoLastX = player.x; autoLastY = player.y;
  }
  int px = (int)player.x, py = (int)player.y;
  autoVisited[py][px] = 1;

  // 0) Unsticking: if we tried to walk but didn't move for 0.8 s, back up
  //    for a moment while turning, then carry on.
  if (autoBackupMs > 0) {
    autoBackupMs -= dtMs;
    in.move = -0.5f; in.turn = 0.6f * autoBackupDir;
    autoLastX = player.x; autoLastY = player.y;
    return;
  }

  // 1) Is a monster in sight? Then face it and hold still so you can shoot.
  //    (Monsters walk up to you, so there's no need to chase them.)
  int target = -1; float bestD = 6.0f;            // only aim at monsters closer than 6 squares
  for (int i = 0; i < thingCount; i++) {
    if (things[i].type != ENEMY) continue;
    float dx = things[i].x - player.x, dy = things[i].y - player.y;
    float d = sqrtf(dx * dx + dy * dy);
    if (d < bestD && lineOfSight(player.x, player.y, things[i].x, things[i].y)) { bestD = d; target = i; }
  }
  if (target >= 0) {
    float want = atan2f(things[target].y - player.y, things[target].x - player.x);
    float diff = wrapAngle(want - player.ang);
    if (fabsf(diff) * bestD < 0.2f) {
      in.turn = 0;                                  // lined up: hold steady, that's your cue to shoot
    } else {
      // Keep turning at least a little (never slower than 0.31) until it's
      // really in the crosshair, so "stopped turning" always means "fire now".
      float t = constrain(diff * 3.0f, -1.0f, 1.0f);
      if (fabsf(t) < 0.31f) t = diff > 0 ? 0.31f : -0.31f;
      in.turn = t;
    }
    autoStuckMs = 0;
    return;
  }

  // 2) Plan: next square on the shortest path to somewhere new. If we've
  //    seen everything, go hunt the monsters that are left.
  //    Once we pick a goal square we stick with it until we get there;
  //    otherwise two goals at the same distance make us shuffle back and forth.
  int next = -1;
  if (autoGoal >= 0 && !autoVisited[autoGoal / MAP_W][autoGoal % MAP_W]) next = autoNextStep(2);
  if (next < 0) next = autoNextStep(0);
  if (next < 0) next = autoNextStep(1);
  if (next < 0) {                                   // seen it all: start a fresh lap of the map
    memset(autoVisited, 0, sizeof(autoVisited));
    autoVisited[py][px] = 1;
    next = autoNextStep(0);
  }
  if (next < 0) { in.turn = 0.4f; return; }         // nowhere to go at all: just look around
  int nx = next % MAP_W, ny = next / MAP_W;
  float cx = px + 0.5f, cy = py + 0.5f;             // center of the square we're in
  float tx = nx + 0.5f, ty = ny + 0.5f;             // center of the next square

  // How far are we off the straight line between the two square centers?
  float offLine = (nx != px) ? fabsf(player.y - cy) : fabsf(player.x - cx);

  if (worldMap[ny][nx] == 9) {
    // 3) The next square is a DOOR. First line up in the middle of our
    //    square, then face the door's center. Doors open by themselves
    //    when you face them up close, so once we're lined up it slides open.
    float dcx = player.x - cx, dcy = player.y - cy;
    if (dcx * dcx + dcy * dcy > 0.15f * 0.15f) {
      autoSteerTo(in, cx, cy, 0.5f);
    } else {
      float diff = wrapAngle(atan2f(ty - player.y, tx - player.x) - player.ang);
      in.turn = constrain(diff * 3.0f, -1.0f, 1.0f);
    }
  } else if (offLine > 0.18f) {
    // 4) Drifted sideways? Get back to the middle of our square first,
    //    so we don't clip a wall corner (or a door frame) on the way.
    autoSteerTo(in, cx, cy, 0.5f);
  } else {
    // 5) Walk to the next square's center. Aim a bit further along the same
    //    direction so we keep going straight through doorways.
    float ax = tx + (nx - px) * 0.5f, ay = ty + (ny - py) * 0.5f;
    autoSteerTo(in, ax, ay, 0.8f);
  }

  // Stuck check: trying to walk but not getting anywhere?
  float movedSq = (player.x - autoLastX) * (player.x - autoLastX) + (player.y - autoLastY) * (player.y - autoLastY);
  if (in.move > 0.1f && movedSq < 0.0001f) autoStuckMs += dtMs; else autoStuckMs = 0;
  autoLastX = player.x; autoLastY = player.y;
  if (autoStuckMs > 800) { autoBackupMs = 400; autoBackupDir = -autoBackupDir; autoStuckMs = 0; }
}

// ---------------------------------------------------------------------
//  UPDATE: move everything forward in time by dtMs milliseconds
// ---------------------------------------------------------------------
static void update(const Input& in, int dtMs) {
  float dt = dtMs / 1000.0f;   // milliseconds -> seconds
  stateMs += dtMs;
  if (messageMs > 0) messageMs -= dtMs;

  if (state == TITLE) {
    titleAng += 0.4f * dt;     // slowly spin the camera behind the title
    if (in.firePressed || in.usePressed) { newGame(); state = PLAY; stateMs = 0; }
    return;
  }
  if (state == DEAD || state == WIN) {
    if (stateMs > 1000 && (in.firePressed || in.usePressed)) { state = TITLE; stateMs = 0; }
    return;
  }

  // ---- turning and walking (from tilt) ----
  player.ang += in.turn * TURN_SPEED * dt;
  if (player.ang > 6.2831853f) player.ang -= 6.2831853f;  // keep angle in 0..2*pi
  if (player.ang < 0) player.ang += 6.2831853f;
  float c = cosf(player.ang), s = sinf(player.ang);
  float step = in.move * MOVE_SPEED * dt;
  tryMove(player.x, player.y, c * step, s * step, PLAYER_RADIUS);
  bobPhase += fabsf(in.move) * dt * 10.0f;

  // ---- shooting ----
  if (fireCooldown > 0) fireCooldown -= dtMs;
  if (flashMs > 0) flashMs -= dtMs;
  if ((in.firePressed || in.fireHeld) && fireCooldown <= 0) {
    if (player.ammo > 0) {
      player.ammo--;
      flashMs = 90;
      fireCooldown = FIRE_DELAY_MS;
      shoot();
    } else if (in.firePressed) {
      showMessage("NO AMMO");
    }
  }

  // ---- automatic doors ----
  // Every frame, check the square(s) right in front of you. If there's a
  // door within reach (about 1 square), it slides open by itself - no
  // button needed.
  useAction();

  // ---- monsters and items ----
  if (hurtMs > 0) hurtMs -= dtMs;
  for (int i = 0; i < thingCount; i++) {
    Thing& t = things[i];
    float dx = player.x - t.x, dy = player.y - t.y;
    float d = sqrtf(dx * dx + dy * dy);
    t.dist = d;
    if (t.hurtMs > 0) t.hurtMs -= dtMs;

    if (t.type == HEALTH && d < 0.5f && player.hp < START_HP) {
      player.hp += 25; if (player.hp > START_HP) player.hp = START_HP;
      t.type = USED; showMessage("+25 HEALTH");
    } else if (t.type == AMMO && d < 0.5f) {
      player.ammo += 12; t.type = USED; showMessage("+12 AMMO");
    } else if (t.type == ENEMY) {
      if (!t.awake && d < ENEMY_SIGHT && lineOfSight(t.x, t.y, player.x, player.y)) t.awake = true;
      if (t.awake && d > 0.8f && t.hurtMs <= 0) {              // walk toward you
        float mv = ENEMY_SPEED * dt / d;
        tryMove(t.x, t.y, dx * mv, dy * mv, 0.25f);
      }
      if (d < 1.1f) {                                          // close enough to claw you
        t.attackMs -= dtMs;
        if (t.attackMs <= 0) {
          player.hp -= 6 + (int)(rnd() % 7);
          hurtMs = 220;
          t.attackMs = 1000;
        }
      }
    }
  }

  if (player.hp <= 0) { player.hp = 0; state = DEAD; stateMs = 0; }
  else if (player.kills >= totalEnemies) { state = WIN; stateMs = 0; }
}

// ---------------------------------------------------------------------
//  RENDER STEP 1: floor, ceiling and walls (the raycasting part)
// ---------------------------------------------------------------------
//  Fixed-point reminder: numbers ending in "Q" are real value * 65536.
static const int32_t ONE = 65536;

// 1 / |v| in fixed point, capped so it never overflows (a ray going almost
// exactly sideways would otherwise give "infinity").
static inline int32_t invAbsQ(int32_t vQ) {
  if (vQ < 0) vQ = -vQ;
  if (vQ < 4) return 0x3FFFFFFF;                 // basically infinite
  int64_t r = ((int64_t)1 << 32) / vQ;
  return r > 0x3FFFFFFF ? 0x3FFFFFFF : (int32_t)r;
}

static void renderWalls(float camX, float camY, float camAng) {
  // Wall height scale: for square pixels, a 1-square-tall wall at distance 1
  // should be as tall as the screen is "wide per unit of view".
  const int32_t wallScale = (int32_t)(W * 0.5f / FOV_PLANE);

  // Player position and view direction converted to fixed-point once.
  int32_t posXQ = (int32_t)(camX * ONE), posYQ = (int32_t)(camY * ONE);
  float c = cosf(camAng), s = sinf(camAng);
  int32_t dirXQ = (int32_t)(c * ONE), dirYQ = (int32_t)(s * ONE);
  // The "camera plane" is a line sideways to your view; it sets the field of view.
  int32_t planeXQ = (int32_t)(-s * FOV_PLANE * ONE), planeYQ = (int32_t)(c * FOV_PLANE * ONE);

  const int half = H / 2;

  for (int x = 0; x < W; x++) {
    // Where is this column across the screen? -1 = left edge, +1 = right edge.
    int32_t camQ = (int32_t)(((int64_t)(2 * x - W + 1) * ONE) / W);
    // Direction of this column's ray.
    int32_t rayXQ = dirXQ + (int32_t)(((int64_t)planeXQ * camQ) >> 16);
    int32_t rayYQ = dirYQ + (int32_t)(((int64_t)planeYQ * camQ) >> 16);

    // Which map square are we standing in?
    int mapX = posXQ >> 16, mapY = posYQ >> 16;

    // How far the ray travels to cross ONE whole square in X (and in Y).
    // = 1 / |rayX|. In fixed point that is 2^32 / rayXQ.
    int32_t deltaXQ = invAbsQ(rayXQ);
    int32_t deltaYQ = invAbsQ(rayYQ);

    // Which way to step through the grid, and distance to the FIRST grid line.
    int stepX, stepY;
    int64_t sideXQ, sideYQ;
    int32_t fracX = posXQ & 0xFFFF, fracY = posYQ & 0xFFFF;   // position inside the square
    if (rayXQ < 0) { stepX = -1; sideXQ = ((int64_t)fracX * deltaXQ) >> 16; }
    else           { stepX = 1;  sideXQ = ((int64_t)(ONE - fracX) * deltaXQ) >> 16; }
    if (rayYQ < 0) { stepY = -1; sideYQ = ((int64_t)fracY * deltaYQ) >> 16; }
    else           { stepY = 1;  sideYQ = ((int64_t)(ONE - fracY) * deltaYQ) >> 16; }

    // DDA ("digital differential analyzer"): hop from grid line to grid line
    // until we land in a wall square. Always take the nearer hop.
    int side = 0;          // 0 = hit a wall facing east/west, 1 = north/south
    uint8_t tile = 1;
    for (int guard = 0; guard < 40; guard++) {
      if (sideXQ < sideYQ) { sideXQ += deltaXQ; mapX += stepX; side = 0; }
      else                 { sideYQ += deltaYQ; mapY += stepY; side = 1; }
      if (mapX < 0 || mapY < 0 || mapX >= MAP_W || mapY >= MAP_H) { tile = 1; break; }
      tile = worldMap[mapY][mapX];
      if (tile) break;
    }

    // Straight-ahead distance to the wall (not the slanted ray length,
    // otherwise walls look bent like a fish-eye lens).
    int64_t perpQ = side == 0 ? sideXQ - deltaXQ : sideYQ - deltaYQ;
    if (perpQ < ONE / 32) perpQ = ONE / 32;                 // avoid divide-by-tiny
    zbuf[x] = (int32_t)perpQ;

    // How tall is the wall stripe on screen?
    int lineH = (int)(((int64_t)wallScale << 16) / perpQ);
    int top = half - lineH / 2;
    int bottom = top + lineH;

    // Exactly where along the wall did we hit? (0..1) -> which texture column.
    int32_t hitQ = side == 0 ? posYQ + (int32_t)((perpQ * rayYQ) >> 16)
                             : posXQ + (int32_t)((perpQ * rayXQ) >> 16);
    int texX = (hitQ & 0xFFFF) >> 12;                       // 0..15
    if ((side == 0 && rayXQ > 0) || (side == 1 && rayYQ < 0)) texX = TEX - 1 - texX;

    // Brightness: close = bright, far = dark. N/S walls a bit darker for depth.
    int light = 280 - (int)((perpQ * 22) >> 16);            // lose ~22 brightness per square
    if (light < 50) light = 50;
    if (light > 256) light = 256;
    if (side == 1) light = light * 3 / 4;

    const uint16_t* texCol = &textures[texFor(tile)][0];
    int32_t texStepQ = (TEX << 16) / (lineH ? lineH : 1);   // texture pixels per screen pixel
    int y0 = top < 0 ? 0 : top;
    int y1 = bottom > H ? H : bottom;
    int32_t texPosQ = (y0 - top) * texStepQ;

    uint16_t* p = fb + x;  // walk down this column
    for (int y = 0; y < y0; y++, p += W) {                  // ceiling
      int k = 40 + (half - y) * 60 / half;                  // brighter at the top
      *p = rgb(k / 3, k / 3, k / 2 + 10);
    }
    for (int y = y0; y < y1; y++, p += W) {                 // wall
      int texY = (texPosQ >> 16) & (TEX - 1);
      texPosQ += texStepQ;
      *p = shade(texCol[texY * TEX + texX], light);
    }
    for (int y = y1; y < H; y++, p += W) {                  // floor
      int k = 30 + (y - half) * 110 / half;                 // brighter near your feet
      *p = rgb(k * 3 / 4, k / 2, k / 4);
    }
  }
}

// ---------------------------------------------------------------------
//  RENDER STEP 2: monsters and items (billboard sprites)
// ---------------------------------------------------------------------
static void renderSprites(float camX, float camY, float camAng) {
  const float wallScale = W * 0.5f / FOV_PLANE;
  float c = cosf(camAng), s = sinf(camAng);

  // Sort far-to-near so close things are drawn on top (simple bubble sort,
  // fine for a couple dozen things).
  int order[MAX_THINGS];
  float depthOf[MAX_THINGS];
  int n = 0;
  for (int i = 0; i < thingCount; i++) {
    if (things[i].type == USED) continue;
    float dx = things[i].x - camX, dy = things[i].y - camY;
    depthOf[i] = dx * c + dy * s;
    order[n++] = i;
  }
  for (int a = 0; a < n; a++)
    for (int b = a + 1; b < n; b++)
      if (depthOf[order[b]] > depthOf[order[a]]) { int t = order[a]; order[a] = order[b]; order[b] = t; }

  for (int k = 0; k < n; k++) {
    Thing& t = things[order[k]];
    float dx = t.x - camX, dy = t.y - camY;
    float depth = depthOf[order[k]];           // distance straight ahead
    float side = -dx * s + dy * c;             // distance to the right
    if (depth < 0.15f) continue;               // behind you or too close

    // Project onto the screen: further away = smaller and closer to the center.
    int screenX = (int)(W / 2 + side / (depth * FOV_PLANE) * (W / 2));
    int size = (int)(wallScale / depth);       // a 1-square-tall object
    if (size < 1) continue;
    int floorY = H / 2 + size / 2;             // where the floor is at that distance
    int left = screenX - size / 2;
    int top = floorY - size;

    const char* const* art = (t.type == ENEMY) ? IMP_ART
                           : (t.type == CORPSE) ? CORPSE_ART
                           : (t.type == HEALTH) ? HEALTH_ART : AMMO_ART;
    int32_t depthQ = (int32_t)(depth * ONE);
    int light = 256 - (int)(depth * 18);
    if (light < 70) light = 70;
    bool flashWhite = (t.type == ENEMY && t.hurtMs > 0);

    // Little hop while monsters walk toward you
    if (t.type == ENEMY && t.awake) top -= ((stateMs / 160) & 1) * size / 24;

    for (int sx = 0; sx < size; sx++) {
      int x = left + sx;
      if (x < 0 || x >= W) continue;
      if (zbuf[x] < depthQ) continue;          // a wall is in front: hidden
      int artX = sx * 16 / size;
      for (int sy = 0; sy < size; sy++) {
        int y = top + sy;
        if (y < 0 || y >= H) continue;
        int artY = sy * 16 / size;
        uint16_t col = colorOf(art[artY][artX]);
        if (col == TRANSPARENT) continue;
        fb[y * W + x] = flashWhite ? 0xFFFF : shade(col, light);
      }
    }
  }
}

// ---------------------------------------------------------------------
//  RENDER STEP 3: your gun, crosshair and HUD text
// ---------------------------------------------------------------------
static void renderHud() {
  bool big = (W >= 128);                  // TFT = big, tiny OLED = small
  int gs = big ? 2 : 1;                   // gun pixel size
  int gunW = 16 * gs, gunH = 12 * gs;
  int bobX = (int)(sinf(bobPhase) * 3 * gs);
  int bobY = (int)(fabsf(cosf(bobPhase)) * 2 * gs);
  int gx = W / 2 - gunW / 2 + bobX;
  int gy = H - gunH + bobY + (flashMs > 0 ? gs : 0);   // kick down when firing
  if (flashMs > 0) drawArt(FLASH_ART, 8, gx + 2 * gs, gy - 8 * gs + 2 * gs, gs);
  drawArt(GUN_ART, 12, gx, gy, gs);

  // crosshair: a tiny plus in the middle
  uint16_t ch = rgb(255, 255, 255);
  putPixel(W / 2, H / 2 - 2, ch); putPixel(W / 2, H / 2 + 2, ch);
  putPixel(W / 2 - 2, H / 2, ch); putPixel(W / 2 + 2, H / 2, ch);

  char buf[16];
  // health top-left (turns red when low), ammo under it, kills top-right
  snprintf(buf, sizeof(buf), "HP%d", player.hp);
  drawText(1, 1, buf, player.hp > 30 ? rgb(120, 255, 120) : rgb(255, 60, 60), 1);
  snprintf(buf, sizeof(buf), "AM%d", player.ammo);
  drawText(1, 8, buf, rgb(255, 220, 80), 1);
  snprintf(buf, sizeof(buf), "K%d/%d", player.kills, totalEnemies);
  drawText(W - textWidth(buf, 1) - 2, 1, buf, rgb(255, 255, 255), 1);

  if (messageMs > 0) drawTextCentered(H / 2 + 5, message, rgb(255, 255, 160), 1);
}

// Tint the whole screen red (ouch!).
static void tintScreen(int amount256) {
  for (int i = 0; i < W * H; i++) fb[i] = tintRed(fb[i], amount256);
}

// ---------------------------------------------------------------------
//  TITLE LOGO: big chunky "DOOM" letters, drawn entirely with code
// ---------------------------------------------------------------------
//  This is our OWN lettering in a DOOM-*style* (it is not the real logo
//  artwork, and instead of the "id" mark we put our own AUG badge).
//  How it's built, layer by layer:
//   1) Each letter is a tiny grid of blocks. '#' = solid block, and the
//      letters a/b/c/d are HALF blocks cut diagonally, which gives the
//      letters chamfered (angled) corners like cut sheet metal.
//   2) "Perspective": letters get TALLER toward the left and right edges
//      (mostly upward), so the word looks like it's flying out at you.
//   3) Layers, back to front: drop shadow, black outline, a deep 3D
//      "extrusion" (the underside of the metal), then the front face with
//      a yellow -> orange -> red -> brown fade, brushed-metal streaks and a
//      light/dark bevel on the edges so it looks chiseled.
//   4) A little red metal plaque that says AUG in the lower-right corner.
//  On the tiny black & white OLED we use white letters with a dotted
//  (gray) 3D side instead, because red turns into almost nothing there.
//
//  Half-block legend (picture the block split corner to corner):
//    a = top-left corner cut off     b = top-right corner cut off
//    c = bottom-left corner cut off  d = bottom-right corner cut off
static const char* const LOGO_D[9] = {
  "######b.",
  "#######b",
  "###..###",
  "###..###",
  "###..###",
  "###..###",
  "###..###",
  "#######d",
  "######d.",
};
static const char* const LOGO_O[9] = {
  ".a####b.",
  "a######b",
  "###..###",
  "###..###",
  "###..###",
  "###..###",
  "###..###",
  "c######d",
  ".c####d.",
};
static const char* const LOGO_M[9] = {
  "###b..a###",
  "####ba####",
  "##########",
  "###c##d###",
  "###.cd.###",
  "###....###",
  "###....###",
  "###....###",
  "###....###",
};
static const int LOGO_ROWS = 9;
static const int LOGO_COLS = 8 + 1 + 8 + 1 + 8 + 1 + 10;  // letters + 1-block gaps = 37

// Is the point (fx, fy) inside the word "DOOM"? fx, fy are measured in
// blocks and can have a fraction (0.5 = halfway across a block). The
// fraction is what lets us draw the diagonal half blocks.
static bool logoSolid(float fx, float fy) {
  if (fy < 0 || fy >= LOGO_ROWS || fx < 0) return false;
  int cx = (int)fx, cy = (int)fy;
  float u = fx - cx, v = fy - cy;            // where inside the block (0..1)
  const char* const* letters[4] = {LOGO_D, LOGO_O, LOGO_O, LOGO_M};
  const int widths[4] = {8, 8, 8, 10};
  for (int i = 0; i < 4; i++) {
    if (cx < widths[i]) {
      switch (letters[i][cy][cx]) {
        case '#': return true;
        case 'a': return u + v >= 1.0f;      // keep the bottom-right half
        case 'b': return v >= u;             // keep the bottom-left half
        case 'c': return u >= v;             // keep the top-right half
        case 'd': return u + v <= 1.0f;      // keep the top-left half
        default:  return false;              // '.' = empty
      }
    }
    cx -= widths[i];
    if (cx == 0) return false;   // the gap between letters
    cx -= 1;
  }
  return false;
}

// The logo is "stamped" into this mask once (0 = empty, 1..255 = front face
// of a letter, and the number is how far down the letter we are, for the
// color fade), so drawing every frame is just fast lookups.
static const int LOGO_MW = 160, LOGO_MH = 56;
static uint8_t logoMask[LOGO_MH][LOGO_MW];   // 9 KB, fits easily in the C3's RAM
static int logoBuiltFor = -1;     // which screen width the mask was built for
static int logoTop, logoBottom;   // first and last screen row the letters use
static int logoRight;             // right-most column the letters use

static inline bool logoAt(int x, int y) {          // x,y = screen pixel
  if (x < 0 || y < 0 || x >= LOGO_MW || y >= LOGO_MH) return false;
  return logoMask[y][x] != 0;
}

static void buildLogo(bool big) {
  int lw = big ? 150 : 68;                         // logo width in pixels
  int lh = big ? 32 : 14;                          // logo height in the middle
  int x0 = (W - lw) / 2;                           // left edge, so it's centered
  int anchorY = big ? 28 : 12;                     // this row stays put while the edges grow
  memset(logoMask, 0, sizeof(logoMask));
  logoTop = H; logoBottom = 0; logoRight = 0;
  for (int y = 0; y < LOGO_MH && y < H; y++) {
    for (int x = 0; x < lw && x + x0 < LOGO_MW; x++) {
      // Perspective: 100% tall in the middle, up to 128% at the far edges
      // (squared, so the curve is gentle in the middle and strong at the ends).
      float side = (x - lw * 0.5f) / (lw * 0.5f);  // -1 (left) .. +1 (right)
      float scale = 1.0f + 0.28f * side * side;
      // Undo the stretch to find which letter row this pixel shows. The
      // anchor sits 62% of the way down, so the TOP edge flares out more
      // than the bottom - that gives the classic "arched" look.
      float fy = ((y - anchorY) / scale + lh * 0.62f) * LOGO_ROWS / lh;
      float fx = (x + 0.5f) * LOGO_COLS / lw;
      if (logoSolid(fx, fy)) {
        int t = (int)(fy * 255 / LOGO_ROWS);       // 0 at the top of the letter, 255 at the bottom
        logoMask[y][x + x0] = (uint8_t)(1 + (t > 254 ? 254 : t));
        if (y < logoTop) logoTop = y;
        if (y > logoBottom) logoBottom = y;
        if (x + x0 > logoRight) logoRight = x + x0;
      }
    }
  }
  logoBuiltFor = W;
}

// Front-face metal color: yellow at the top, then orange, red, and a dark
// rusty brown at the very bottom. t255 = 0 (top) .. 254 (bottom).
static uint16_t logoGradient(int t255) {
  // Four color "stops"; we blend between the two nearest ones.
  static const uint8_t stops[4][3] = {
    {255, 238, 110},   // hot yellow
    {255, 150, 20},    // orange
    {205, 40, 10},     // red
    {135, 28, 8},      // rusty brown
  };
  int seg = t255 * 3 / 255;  if (seg > 2) seg = 2;  // which pair of stops
  int k = t255 * 3 - seg * 255;                     // 0..255 between them
  const uint8_t* a = stops[seg]; const uint8_t* b = stops[seg + 1];
  return rgb(a[0] + (b[0] - a[0]) * k / 255,
             a[1] + (b[1] - a[1]) * k / 255,
             a[2] + (b[2] - a[2]) * k / 255);
}

// A tiny "random" number from a position, always the same for the same
// spot. We use it for brushed-metal streaks on the letters.
static inline int logoNoise(int x, int y) {
  uint32_t h = (uint32_t)(x * 374761393u + y * 668265263u);
  h = (h ^ (h >> 13)) * 1274126177u;
  return (int)((h >> 24) & 31) - 16;               // -16 .. +15
}

// Where the 3D side of the letters goes: straight DOWN, drifting a little
// right, so it looks like we're looking up at a slab of metal.
static inline bool extrusionAt(int x, int y, int depth, int* hit) {
  for (int d = 1; d <= depth; d++)
    if (logoAt(x - d / 3, y - d)) { *hit = d; return true; }
  return false;
}

static void drawLogo(bool big) {
  if (logoBuiltFor != W) buildLogo(big);
  const int depth = big ? 5 : 1;                    // how thick the 3D side is
  const int bevel = big ? 2 : 1;                    // how wide the shiny edges are
  for (int y = logoTop - 2; y <= logoBottom + depth + 3 && y < H; y++) {
    if (y < 0) continue;
    for (int x = 0; x < W; x++) {
      uint16_t* p = &fb[y * W + x];
      int m = logoAt(x, y) ? logoMask[y][x] : 0;
      if (m) {
        // ---- front face ----
        if (!big) { *p = rgb(255, 255, 255); continue; }  // OLED: solid white
        uint16_t c = logoGradient(m - 1);
        // Bevel: pixels near the top/left edge catch the light, pixels near
        // the bottom/right edge are in shadow. (Checking 1 and 2 pixels away.)
        bool lightEdge = false, darkEdge = false;
        for (int d = 1; d <= bevel; d++) {
          if (!logoAt(x, y - d) || !logoAt(x - d, y)) lightEdge = true;
          if (!logoAt(x, y + d) || !logoAt(x + d, y)) darkEdge = true;
        }
        if (lightEdge && !logoAt(x, y - 1)) c = rgb(255, 250, 205);   // top rim: almost white
        else if (lightEdge) c = shade(c, 256) | 0x18E3;               // left rim: brighter
        else if (darkEdge) c = shade(c, 120);                         // bottom/right rim: dark
        else {
          // Brushed metal: each short horizontal streak is a bit lighter or darker.
          int n = logoNoise(x >> 2, y);
          c = shade(c, 232 + n);
        }
        *p = c;
        continue;
      }
      // ---- 3D side (extrusion): dark red-brown, darker further back ----
      int ext;
      if (extrusionAt(x, y, depth, &ext)) {
        *p = big ? shade(rgb(110, 22, 8), 250 - ext * 34)
                 : rgb(109, 109, 109);                 // OLED: exact 50% gray = neat checkerboard
        continue;
      }
      // ---- outline: black ring around the face and the side ----
      bool nearLogo = false;
      for (int dy = -1; dy <= 1 && !nearLogo; dy++)
        for (int dx = -1; dx <= 1 && !nearLogo; dx++) {
          int dummy;
          if (logoAt(x + dx, y + dy) || extrusionAt(x + dx, y + dy, depth, &dummy)) nearLogo = true;
        }
      if (nearLogo) { *p = 0x0000; continue; }
      // ---- soft drop shadow on the background ----
      if (logoAt(x - 3, y - depth - 2)) *p = shade(*p, 70);
    }
  }
}

// Our own badge (in the spot where the real game has its maker's mark):
// a small red metal plaque with "AUG" stamped on it.
static void drawAugBadge(bool big) {
  const int bw = 15, bh = 9;                        // plaque size: 11px text + padding
  int bx, by;
  if (big) { bx = logoRight - bw + 2; by = logoBottom + 1; }   // tucked under the M's right leg
  else     { bx = W - bw - 1;          by = H - bh - 1; }      // OLED: bottom-right corner
  // black border all around (1 px), so it pops off the background
  fillRect(bx - 1, by - 1, bw + 2, bh + 2, 0x0000);
  for (int j = 0; j < bh; j++) {
    for (int i = 0; i < bw; i++) {
      uint16_t c;
      if (!big)                    c = rgb(255, 255, 255);          // OLED: white plaque
      else if (j == 0 || i == 0)   c = rgb(255, 170, 120);          // lit top/left rim
      else if (j == bh - 1 || i == bw - 1) c = rgb(70, 10, 5);      // shadowed bottom/right rim
      else c = shade(rgb(200 - j * 10, 30, 15), 236 + logoNoise(i >> 2, j)); // brushed red metal
      putPixel(bx + i, by + j, c);
    }
  }
  if (big) {
    drawText(bx + 3, by + 3, "AUG", rgb(60, 5, 0), 1, false);       // stamped-in shadow
    drawText(bx + 2, by + 2, "AUG", rgb(255, 225, 150), 1, false);  // the letters
  } else {
    drawText(bx + 2, by + 2, "AUG", 0x0000, 1, false);              // OLED: black on white
  }
}

// ---------------------------------------------------------------------
//  RENDER: draw one complete frame for the current state
// ---------------------------------------------------------------------
static void render() {
  bool big = (W >= 128);
  if (state == TITLE) {
    // The level slowly spins behind the title, dimmed a bit.
    renderWalls(1.5f, 1.5f + 0.0f, titleAng);
    for (int i = 0; i < W * H; i++) fb[i] = shade(fb[i], 110);
    drawLogo(big);                      // the big "DOOM" letters
    if ((stateMs / 500) % 2 == 0)       // blink
      drawTextCentered(big ? 55 : 26, "PRESS A", rgb(255, 255, 255), 1);
    if (big) drawTextCentered(65, "A=FIRE  HOLD B=TURN LEFT", rgb(160, 160, 200), 1);
    drawAugBadge(big);                  // our AUG plaque (replaces the old AUG-TEC text)
    return;
  }

  renderWalls(player.x, player.y, player.ang);
  renderSprites(player.x, player.y, player.ang);

  if (state == PLAY) {
    renderHud();
    if (hurtMs > 0) tintScreen(hurtMs > 110 ? 150 : 80);
  } else if (state == DEAD) {
    tintScreen(170);
    drawTextCentered(big ? 20 : 8, "YOU DIED", rgb(255, 255, 255), big ? 2 : 1);
    if (stateMs > 1000) drawTextCentered(big ? 50 : 26, "PRESS A", rgb(255, 220, 80), 1);
  } else if (state == WIN) {
    for (int i = 0; i < W * H; i++) fb[i] = shade(fb[i], 120);
    drawTextCentered(big ? 16 : 6, "YOU WIN!", rgb(120, 255, 120), big ? 2 : 1);
    char buf[20];
    snprintf(buf, sizeof(buf), "KILLS %d/%d", player.kills, totalEnemies);
    drawTextCentered(big ? 40 : 18, buf, rgb(255, 255, 255), 1);
    if (stateMs > 1000) drawTextCentered(big ? 56 : 28, "PRESS A", rgb(255, 220, 80), 1);
  }
}

// ---------------------------------------------------------------------
//  FOR THE ONBOARD OLED: turn our color frame into black & white dots
// ---------------------------------------------------------------------
//  The OLED pixels are only ON or OFF. To fake gray we use "ordered
//  dithering": a 4x4 pattern of thresholds, so a 50% gray becomes a
//  checkerboard, 25% gray becomes every 4th dot, etc. Your eye blends it.
//  Output is in the SSD1306 "page" layout that U8g2 uses internally:
//  each byte = 8 pixels stacked vertically; byte index = (y/8)*W + x.
static const uint8_t BAYER4[4][4] = {
  { 0, 8, 2, 10}, {12, 4, 14, 6}, { 3, 11, 1, 9}, {15, 7, 13, 5}};

static void ditherToPages(uint8_t* pages) {
  memset(pages, 0, (size_t)W * ((H + 7) / 8));
  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
      uint16_t c = fb[y * W + x];
      // Brightness (luma): green counts most, blue least, like human eyes.
      int r = (c >> 11) << 3, g = ((c >> 5) & 0x3F) << 2, b = (c & 0x1F) << 3;
      int lum = (r * 77 + g * 150 + b * 29) >> 8;          // 0..255
      // Contrast curve: dark stuff (ceiling, far floor) becomes pure black so
      // the walls and monsters stand out instead of drowning in dots.
      lum = (lum - 45) * 2;
      if (lum < 0) lum = 0;
      int threshold = BAYER4[y & 3][x & 3] * 16 + 8;       // 8..248
      if (lum > threshold) pages[(y >> 3) * W + x] |= (uint8_t)(1 << (y & 7));
    }
  }
}

}  // namespace game
