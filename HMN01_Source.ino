// ========== My Day (Nano ESP32 / Teensy 4.x + ST7789 240x135) ==========
// Buttons to GND (internal pullups):
//   A4=UP, A3=OK  (single=toggle [not in MOVE], double=enter/exit MOVE, triple=delete->confirm), A2=DOWN
//
// Changes in this build:
// - Priority Editor takes precedence: tap UP/DOWN = move; hold UP/DOWN = enter [ Prio ] immediately.
// - Hold threshold 300 ms for snappy hold.
// - Priority Editor cycles slowly (~300 ms/step) while held.
// - USB Serial Import/Export retained.
// - Read modal (hold OK ~1.5s) + marquee kept.
// - Delete modal options changed to:  **Y (Up)**   **N (Down)**
// - THEME: Catppuccin Frappé (tuned for ST7789) + Material-ish shadow on pill/modals
// - Text color for active tasks = pure white; completed stays dim.
// - MOVE mode color set to #EED49F.
// - Highlight pill now sized from bracket glyph with equal top/bottom gap.
//
// Teensy 4.x + Nano ESP32 both compile. ESP32 uses Preferences; Teensy has a no-op shim.

#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

#if defined(ESP32)
  #include <Preferences.h>
  Preferences prefs;
#else
  // Minimal no-op persistence shim for Teensy / others
  class Preferences {
  public:
    void   begin(const char*, bool) {}
    void   end() {}
    void   clear() {}
    uint8_t  getUChar(const char*, uint8_t def=0) { return def; }
    void     putUChar(const char*, uint8_t) {}
    uint32_t getUInt(const char*, uint32_t def=0) { return def; }
    void     putUInt(const char*, uint32_t) {}
    String   getString(const char*, const String& def="") { return def; }
    void     putString(const char*, const String&) {}
  } prefs;
#endif

#include <string.h>

// ---------- Text capacity ----------
#define TASK_TEXT_MAX 96

// ---------------- Build-based reseed control ----------------
#define APPLY_SEED_ON_NEW_BUILD 1
static const char* BUILD_ID = __DATE__ " " __TIME__;

// ---------- Display (HW SPI) ----------
#define USE_HARDWARE_SPI 1
#define SPI_SPEED_HZ     60000000

// Default pins (ESP32 Nano-style). Teensy uses hardware pins; we call SPI.begin() without mapping.
#define TFT_CS    9
#define TFT_DC    10
#define TFT_RST   11
#define TFT_MOSI  12
#define TFT_SCLK  13

#if USE_HARDWARE_SPI
  Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);
#else
  Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK, TFT_RST);
#endif

// ---------- Buttons (to GND, active-LOW) ----------
#define BTN_UP    A4
#define BTN_OK    A3
#define BTN_DOWN  A2

// Debounce & multi-click timing
const uint16_t DEBOUNCE_MS   = 20;
const uint16_t MIN_PRESS_MS  = 40;
const uint16_t MIN_RELEASE_MS= 60;
const uint16_t MULTI_MS      = 350; // window to group multi-clicks

// Hold thresholds
const uint16_t HOLD_START_MS   = 300; // snappy hold
const uint16_t REPEAT_PERIOD_S = 90;
const uint16_t REPEAT_PERIOD_F = 30;
const uint32_t REPEAT_ACCEL1   = 600;
const uint32_t REPEAT_ACCEL2   = 1200;

// Priority editor slow cadence
const uint16_t PRIO_REPEAT_MS  = 300; // slower, easier to see red vs orange

// Slide animation
const uint8_t  ANIM_FRAMES   = 6;
const uint8_t  ANIM_DELAY_MS = 12;

// ---- Highlight look ----
const uint8_t  HIL_RADIUS         = 3;
const bool     HIL_BORDER_ENABLED = true;
const bool     HIL_BORDER_ACCENT  = true;   // accent border for clearer edge
const uint8_t  HIL_GAP            = 2;      // <<< NEW: px gap above/below bracket glyph

// ---- Text / glyph tweaks ----
const int8_t   TEXT_Y_TWEAK       = 1;
const int8_t   BRACKET_Y_TWEAK    = 1;
const int8_t   BRACKET_X_BIAS     = 0;

// ========= Completed indicator style =========
enum DoneStyle { DONE_BAR=0, DONE_X, DONE_STAR, DONE_DOT };
const DoneStyle DONE_STYLE = DONE_STAR;

const uint8_t  LINE_THICK    = 2;
const uint8_t  DOT_RADIUS    = 5;

// Bar-only extras
const uint8_t  BAR_MARGIN_X  = 0;
const uint8_t  BAR_THICKNESS = 5;
const uint8_t  BAR_EXPAND_X  = 5;
const uint8_t  BAR_UNDERLAP_L= 4;
const uint8_t  BAR_UNDERLAP_R= 4;

// Right-edge alignment tweak
const int8_t   RIGHT_TWEAK_PX = 2;

// ---------- Layout ----------
const int16_t W = 240, H = 135;
const uint8_t HEADER_HEIGHT = 20;

const uint8_t ROW_HEIGHT    = 24;
const uint8_t VISIBLE_ROWS  = 4;

const int16_t LIST_CANVAS_Y = HEADER_HEIGHT;
const int16_t LIST_CANVAS_H = H - HEADER_HEIGHT;

const uint8_t TOP_PAD = 2;
const int16_t PAD_X  = 12;
const int16_t CHECK_GAP = 8;

// ---------- Colors ----------
uint16_t COL_BG, COL_TEXT, COL_DIM, COL_ACCENT, COL_FRAME, COL_BORDER;
uint16_t COL_FOOT, COL_BLACK, COL_MODAL_PANEL, COL_MOVE, COL_HIL, COL_WHITE, COL_X_HARD;
uint16_t COL_PRI_HIGH, COL_PRI_MED, COL_CLOCK;
uint16_t COL_SHADOW; // NEW: soft shadow color

// ---------- Tasks & Persistence ----------
enum PendState : uint8_t { PEND_NONE=0, PEND_WAIT=1, PEND_FADE=2 };
enum Priority  : uint8_t { PRI_LOW=0, PRI_MED=1, PRI_HIGH=2 };

struct Task {
  char     text[TASK_TEXT_MAX];
  bool     done;
  uint8_t  prio;
  // runtime-only
  uint8_t  pend;
  uint32_t pendStartMs;
  uint32_t animStartMs;
  uint32_t uid;
};

const uint8_t MAX_TASKS = 30;
Task tasks[MAX_TASKS];
uint8_t taskCount = 0;
uint32_t nextUid = 1;

// ---------- Modes ----------
enum Mode : uint8_t { MODE_NORMAL=0, MODE_MOVE=1, MODE_CONFIRM_DELETE=2, MODE_READ=3, MODE_PRIO_EDIT=4 };
Mode mode = MODE_NORMAL;

// Priority edit direction: +1 (↑) when entered from UP, -1 (↓) from DOWN
int8_t prioEditDir = 0;
uint32_t g_prioNextBumpMs = 0;

// ---------- Selection / Scrolling ----------
uint8_t topIndex = 0;
uint8_t selected = 0;

// ---------- Mock Clock ----------
uint8_t startHour = 8, startMin = 0;
uint32_t clockStartMs = 0;

// ---------- Canvases ----------
GFXcanvas16 headerCanvas(W, HEADER_HEIGHT);
GFXcanvas16 listCanvas(W, LIST_CANVAS_H);

// ---------- Input safety / modal state ----------
uint32_t confirmIgnoreUntil = 0;
uint32_t confirmDeleteUid   = 0;
uint32_t inputSquelchUntil  = 0;

// ---------- Pending-complete animation ----------
const uint16_t PENDING_DELAY_MS = 2000;
const uint16_t FADE_ANIM_MS     = 300;

// ---------- OK multi-click accumulators ----------
uint8_t  g_okClicks      = 0;
uint32_t g_okLastClickMs = 0;
uint32_t g_okAnchorUid   = 0;

// ---------- UI persistence debounce ----------
bool      uiDirty       = false;
uint32_t  lastUISaveMs  = 0;
const     uint16_t UI_SAVE_DEBOUNCE_MS = 1000;

// ---------- Read (full-text) hold threshold ----------
const uint16_t READ_HOLD_MS = 1500; // 1.5s

// ---------- Marquee (auto-scroll) ----------
const uint16_t MARQUEE_IDLE_MS       = 7000; // wait 7s on a row before auto-scroll
const uint16_t MARQUEE_SPEED_PX_PER_S= 28;
const uint16_t MARQUEE_GAP_PX        = 24;
const uint16_t MARQUEE_FRAME_MS      = 40;   // ~25 FPS
uint32_t g_selStableSince   = 0;
bool     g_marqueeActive    = false;
uint32_t g_marqueeStartMs   = 0;
uint32_t g_lastMarqueeFrame = 0;
int16_t  g_marqueeOffsetPx  = 0;

// ---------- Serial I/O (import/export) ----------
const uint32_t SERIAL_BAUD = 115200;
static bool    g_serialImport = false;
static char    g_lineBuf[256];
static uint16_t g_lineLen = 0;

// ============ Persistence helpers ============
static inline void markUIDirty() { uiDirty = true; }

void saveUIState() {
  prefs.begin("myday", false);
  prefs.putUChar("sel", selected);
  prefs.putUChar("top", topIndex);
  prefs.putUChar("mode", (uint8_t)MODE_NORMAL);
  prefs.end();
  lastUISaveMs = millis();
  uiDirty = false;
}

int findIndexByUid(uint32_t uid);

void loadUIState() {
  prefs.begin("myday", true);
  uint8_t sel = prefs.getUChar("sel", selected);
  uint8_t top = prefs.getUChar("top", topIndex);
  prefs.end();

  if (taskCount == 0) { selected = 0; topIndex = 0; return; }

  if (sel >= taskCount) sel = taskCount - 1;
  selected = sel;

  if (top > selected) top = selected;
  if (selected >= top + VISIBLE_ROWS) top = selected - VISIBLE_ROWS + 1;
  topIndex = top;
}

// ====== >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>> ======
// ====== >>> EDIT YOUR TASKS HERE (they'll persist) <<< ======
// ====== >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>> ======
struct SeedTask { const char* text; bool done; uint8_t prio; };
const SeedTask SEED[] = {
  // 2 high, 1 medium, rest low
  {"USPS money order",                                      false, PRI_HIGH},
  {"Get groceries",                                         false, PRI_HIGH},
  {"After work walk",                                       false, PRI_MED},
  {"Clean cat litter",                                      true,  PRI_LOW},
  {"Clean car",                                             false, PRI_LOW},
  {"Code tasklist for the day (long sample to test marquee & read modal)", false, PRI_LOW},
};
const uint8_t SEED_COUNT = sizeof(SEED)/sizeof(SEED[0]);
// ============================================================

// --- FNV-1a 32-bit hash over seed list (text + done + prio) ---
static inline uint32_t fnv1a_update(uint32_t h, const uint8_t* d, size_t n){
  while(n--) { h ^= *d++; h *= 16777619UL; }
  return h;
}
uint32_t computeSeedHash() {
  uint32_t h = 2166136261UL;
  for (uint8_t i=0;i<SEED_COUNT;i++) {
    const char* s = SEED[i].text;
    h = fnv1a_update(h, (const uint8_t*)s, strlen(s));
    uint8_t d = SEED[i].done ? 1 : 0;  h = fnv1a_update(h, &d, 1);
    uint8_t p = SEED[i].prio;          h = fnv1a_update(h, &p, 1);
  }
  uint8_t c = SEED_COUNT; h = fnv1a_update(h, &c, 1);
  return h;
}

// Apply seed if changed vs stored hash, OR stored list empty, OR build changed.
void applySeedIfChangedAndLoad() {
  uint32_t newHash = computeSeedHash();

  prefs.begin("myday", false);
  uint32_t oldHash = prefs.getUInt("hash", 0xFFFFFFFFUL);
  uint8_t  storedCnt = prefs.getUChar("count", 0);
  String   storedBuild = prefs.getString("build", "");

  bool buildChanged =
  #if APPLY_SEED_ON_NEW_BUILD
    (storedBuild != String(BUILD_ID));
  #else
    false;
  #endif

  bool mustReseed = buildChanged || (oldHash != newHash) || (storedCnt == 0 && SEED_COUNT > 0);

  if (mustReseed) {
    prefs.clear();
    uint8_t cnt = SEED_COUNT; if (cnt > MAX_TASKS) cnt = MAX_TASKS;
    prefs.putUChar("count", cnt);
    for (uint8_t i=0; i<cnt; ++i) {
      char keyT[8], keyD[8], keyP[8];
      snprintf(keyT, sizeof(keyT), "t%u", i);
      snprintf(keyD, sizeof(keyD), "d%u", i);
      snprintf(keyP, sizeof(keyP), "p%u", i);
      char buf[TASK_TEXT_MAX];
      strncpy(buf, SEED[i].text, sizeof(buf)-1); buf[sizeof(buf)-1] = '\0';
      prefs.putString(keyT, buf);
      prefs.putUChar(keyD, SEED[i].done ? 1 : 0);
      prefs.putUChar(keyP, SEED[i].prio);
    }
    prefs.putUInt("hash", newHash);
    #if APPLY_SEED_ON_NEW_BUILD
      prefs.putString("build", BUILD_ID);
    #endif
  }
  prefs.end();

  // Load into RAM
  prefs.begin("myday", true);
  uint8_t cnt = prefs.getUChar("count", 0);
  if (cnt > MAX_TASKS) cnt = MAX_TASKS;
  taskCount = cnt;
  for (uint8_t i=0; i<taskCount; ++i) {
    char keyT[8], keyD[8], keyP[8];
    snprintf(keyT, sizeof(keyT), "t%u", i);
    snprintf(keyD, sizeof(keyD), "d%u", i);
    snprintf(keyP, sizeof(keyP), "p%u", i);
    String s = prefs.getString(keyT, "");
    uint8_t d = prefs.getUChar(keyD, 0);
    uint8_t p = prefs.getUChar(keyP, PRI_LOW);
    strncpy(tasks[i].text, s.c_str(), sizeof(tasks[i].text)-1);
    tasks[i].text[sizeof(tasks[i].text)-1] = '\0';
    tasks[i].done = (d != 0);
    tasks[i].prio = p;
    tasks[i].pend = PEND_NONE;
    tasks[i].pendStartMs = 0;
    tasks[i].animStartMs = 0;
    tasks[i].uid  = nextUid++;
  }
  prefs.end();

  if (selected >= taskCount) selected = taskCount ? (taskCount - 1) : 0;
  if (topIndex > selected)   topIndex = selected;
}

void saveTasksRuntime() {
  prefs.begin("myday", false);
  prefs.putUChar("count", taskCount);
  for (uint8_t i=0; i<taskCount; ++i) {
    char keyT[8], keyD[8], keyP[8];
    snprintf(keyT, sizeof(keyT), "t%u", i);
    snprintf(keyD, sizeof(keyD), "d%u", i);
    snprintf(keyP, sizeof(keyP), "p%u", i);
    prefs.putString(keyT, tasks[i].text);
    prefs.putUChar(keyD, tasks[i].done ? 1 : 0);
    prefs.putUChar(keyP, tasks[i].prio);
  }
  prefs.end();
}

// ---------- Buttons: Clean FSM ----------
namespace Btn {
  enum EventBits : uint8_t {
    EV_NONE       = 0,
    EV_EDGE_PRESS = 1 << 0,
    EV_EDGE_REL   = 1 << 1,
    EV_CLICK      = 1 << 2,
    EV_HOLD_START = 1 << 3,
    EV_REPEAT     = 1 << 4
  };

  struct FSM {
    uint8_t  pin;
    bool     raw = false;
    bool     debPressed = false;
    bool     armedClick = false;
    uint32_t tLastEdge = 0;
    uint32_t tPressStart = 0;
    uint32_t tReleaseStart = 0;
    uint32_t tHold0 = 0;
    uint32_t nextRpt = 0;
    bool     repeating = false;
    uint8_t  ev = EV_NONE;
  };

  static inline void init(FSM &b) {
    pinMode(b.pin, INPUT_PULLUP);
    b.raw = (digitalRead(b.pin) == LOW);
    b.debPressed = false;
    b.armedClick = false;
    b.tLastEdge = b.tPressStart = b.tReleaseStart = millis();
    b.repeating = false; b.tHold0 = 0; b.nextRpt = 0;
    b.ev = EV_NONE;
  }

  static inline void drain(FSM &b) {
    b.armedClick = false;
    b.debPressed = false;
    b.repeating  = false;
    b.ev = EV_NONE;
  }

  static inline void update(FSM &b) {
    b.ev = EV_NONE;
    bool r = (digitalRead(b.pin) == LOW);
    uint32_t now = millis();
    if (r != b.raw) {
      if (now - b.tLastEdge >= DEBOUNCE_MS) {
        b.raw = r; b.tLastEdge = now;
        if (b.raw)  b.tPressStart = now;
        else        b.tReleaseStart = now;
      }
    }
    if (!b.debPressed && b.raw && (now - b.tPressStart >= MIN_PRESS_MS)) {
      b.debPressed = true;
      b.armedClick = true;
      b.repeating  = false;
      b.tHold0     = now;
      b.nextRpt    = now + HOLD_START_MS;
      b.ev |= EV_EDGE_PRESS;
    }
    if (b.debPressed && !r && (now - b.tReleaseStart >= MIN_RELEASE_MS)) {
      b.debPressed = false;
      b.repeating  = false;
      b.ev |= EV_EDGE_REL;
      if (b.armedClick) { b.armedClick = false; b.ev |= EV_CLICK; }
    }
    if (b.debPressed) {
      if (!b.repeating) {
        if ((int32_t)(now - b.nextRpt) >= 0) {
          b.repeating = true;
          b.nextRpt   = now + REPEAT_PERIOD_S;
          b.ev |= EV_HOLD_START;
          b.ev |= EV_REPEAT; // first repeat tick at hold start
        }
      } else {
        uint32_t held = now - b.tHold0;
        uint16_t period = (held >= REPEAT_ACCEL2) ? REPEAT_PERIOD_F
                           : (held >= REPEAT_ACCEL1) ? (REPEAT_PERIOD_S * 2) / 3
                                                      : REPEAT_PERIOD_S;
        if ((int32_t)(now - b.nextRpt) >= 0) {
          b.nextRpt = now + period;
          b.ev |= EV_REPEAT;
        }
      }
    }
  }

  static inline bool pressed(const FSM& b)   { return b.ev & EV_EDGE_PRESS; }
  static inline bool released(const FSM& b)  { return b.ev & EV_EDGE_REL; }
  static inline bool clicked(const FSM& b)   { return b.ev & EV_CLICK; }
  static inline bool repeat(const FSM& b)    { return b.ev & EV_REPEAT; }
  static inline bool holdStart(const FSM& b) { return b.ev & EV_HOLD_START; }
  static inline bool heldFor(const FSM& b, uint32_t ms) {
    return b.debPressed && (millis() - b.tPressStart >= ms);
  }
  static inline uint32_t lastPressDurationMs(const FSM& b) {
    return (b.tReleaseStart >= b.tPressStart) ? (b.tReleaseStart - b.tPressStart) : 0;
  }
}

Btn::FSM bUp{BTN_UP}, bOk{BTN_OK}, bDown{BTN_DOWN};

// ---------- Color helpers ----------
static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((uint16_t)((r * 31 + 127) / 255) << 11) |
         ((uint16_t)((g * 63 + 127) / 255) << 5)  |
         ((uint16_t)((b * 31 + 127) / 255));
}
static inline void toRGB8(uint16_t c, uint8_t &r, uint8_t &g, uint8_t &b) {
  r = ((c >> 11) & 0x1F) * 255 / 31;
  g = ((c >> 5)  & 0x3F) * 255 / 63;
  b = ( c        & 0x1F) * 255 / 31;
}
static inline uint16_t fade565(uint16_t fg, uint16_t bg, uint8_t alpha) {
  uint8_t rF,gF,bF, rB,gB,bB;
  toRGB8(fg, rF,gF,bF); toRGB8(bg, rB,gB,bB);
  uint8_t r = (uint16_t(rF) * alpha + uint16_t(rB) * (255 - alpha) + 127) / 255;
  uint8_t g = (uint16_t(gF) * alpha + uint16_t(gB) * (255 - alpha) + 127) / 255;
  uint8_t b = (uint16_t(bF) * alpha + uint16_t(bB) * (255 - alpha) + 127) / 255;
  return rgb565(r,g,b);
}

// ---------- Header ----------
void formatTime(char* out, size_t n, uint8_t h24, uint8_t m) {
  bool pm = (h24 >= 12);
  uint8_t h12 = h24 % 12; if (h12 == 0) h12 = 12;
  snprintf(out, n, "%u:%02u %s", h12, m, pm ? "PM" : "AM");
}

void rebuildHeader(bool force=false) {
  static char lastTime[12] = "";
  static Mode lastMode = MODE_NORMAL;

  uint32_t elapsedMs = millis() - clockStartMs;
  uint32_t totalMin  = startHour * 60UL + startMin + (elapsedMs / 60000UL);
  uint8_t h = (totalMin / 60U) % 24U;
  uint8_t m = (uint8_t)(totalMin % 60U);

  char nowStr[12]; formatTime(nowStr, sizeof(nowStr), h, m);
  if (!force && (strcmp(nowStr, lastTime) == 0) && (lastMode == mode)) return;

  headerCanvas.fillRect(0,0,W,HEADER_HEIGHT, COL_BG);
  headerCanvas.setTextWrap(false);
  headerCanvas.setTextSize(2);
  headerCanvas.setCursor(PAD_X, 2);

  if (mode == MODE_MOVE) {
    headerCanvas.setTextColor(COL_WHITE, COL_BG);  headerCanvas.print("[ ");
    headerCanvas.setTextColor(COL_MOVE,  COL_BG);  headerCanvas.print("Move");
    headerCanvas.setTextColor(COL_WHITE, COL_BG);  headerCanvas.print(" ]");
  } else if (mode == MODE_CONFIRM_DELETE) {
    headerCanvas.setTextColor(COL_WHITE,       COL_BG);  headerCanvas.print("[ ");
    headerCanvas.setTextColor(COL_MODAL_PANEL, COL_BG);  headerCanvas.print("Delete");
    headerCanvas.setTextColor(COL_WHITE,       COL_BG);  headerCanvas.print(" ]");
  } else if (mode == MODE_READ) {
    headerCanvas.setTextColor(COL_WHITE, COL_BG);  headerCanvas.print("[ ");
    headerCanvas.setTextColor(COL_HIL,   COL_BG);  headerCanvas.print("View");
    headerCanvas.setTextColor(COL_WHITE, COL_BG);  headerCanvas.print(" ]");
  } else if (mode == MODE_PRIO_EDIT) {
    headerCanvas.setTextColor(COL_WHITE, COL_BG);  headerCanvas.print("[ ");
    headerCanvas.setTextColor(COL_HIL,   COL_BG);  headerCanvas.print("Prio");
    headerCanvas.setTextColor(COL_WHITE, COL_BG);  headerCanvas.print(" ]");
  } else {
    headerCanvas.setTextColor(COL_WHITE, COL_BG);  headerCanvas.print("[ ");
    headerCanvas.setTextColor(COL_HIL,   COL_BG);  headerCanvas.print("Task");
    headerCanvas.setTextColor(COL_WHITE, COL_BG);  headerCanvas.print(" ]");
  }

  int16_t x1,y1; uint16_t w,hb;
  headerCanvas.getTextBounds(nowStr, 0, 0, &x1, &y1, &w, &hb);
  int16_t tx = W - PAD_X - (int16_t)w + RIGHT_TWEAK_PX;
  headerCanvas.setCursor(tx, 2);
  headerCanvas.setTextColor(COL_CLOCK, COL_BG);
  headerCanvas.print(nowStr);

  tft.drawRGBBitmap(0, 0, headerCanvas.getBuffer(), W, HEADER_HEIGHT);
  strncpy(lastTime, nowStr, sizeof(lastTime)); lastTime[sizeof(lastTime)-1] = '\0';
  lastMode = mode;
}

// ---------- Footer counts ----------
uint16_t countDone() { uint16_t k=0; for (uint8_t i=0;i<taskCount;i++) if (tasks[i].done) k++; return k; }
uint16_t countLeft() { return (taskCount >= countDone()) ? (taskCount - countDone()) : 0; }

// ---------- Priority helpers ----------
enum Priority : uint8_t; // forward already defined above
inline uint16_t colorForPriority(uint8_t pr) {
  if (pr == PRI_HIGH) return COL_PRI_HIGH;
  if (pr == PRI_MED)  return COL_PRI_MED;
  return COL_WHITE;
}

// ---- Compute textX & available width for task rows ----
void computeTextArea(int16_t &textX, int16_t &availW) {
  listCanvas.setTextSize(2);
  int16_t lbx,lby, xbx,xby, rbx,rby, sbx,sby;
  uint16_t lW,lH, xW,xH, rW,rH, sW,sH;
  listCanvas.getTextBounds("[", 0, 0, &lbx, &lby, &lW, &lH);
  listCanvas.getTextBounds("X", 0, 0, &xbx, &xby, &xW, &xH);
  listCanvas.getTextBounds("]", 0, 0, &rbx, &rby, &rW, &rH);
  listCanvas.getTextBounds(" ", 0, 0, &sbx, &sby, &sW, &sH);
  int16_t insideW = (xW > sW) ? (int16_t)xW : (int16_t)sW;
  insideW += (int16_t)BAR_EXPAND_X; if (insideW < 1) insideW = 1;
  int16_t textX0  = PAD_X + (int16_t)lW + insideW + (int16_t)rW + CHECK_GAP;
  textX  = textX0;
  availW = W - PAD_X - textX0;
  if (availW < 1) availW = 1;
}

// ---------- Row draw ----------
void drawRowContentToCanvas(uint8_t taskIndex, int16_t y, int fadeAlpha, uint16_t fadeBg, int16_t marqueeOffsetPx /*-1 = none*/) {
  if (taskIndex >= taskCount) return;

  listCanvas.setTextWrap(false);
  listCanvas.setTextSize(2);

  // Measure glyphs for brackets/inside
  int16_t lbx,lby, xbx,xby, rbx,rby, sbx,sby;
  uint16_t lW,lH, xW,xH, rW,rH, sW,sH;
  listCanvas.getTextBounds("[", 0, 0, &lbx, &lby, &lW, &lH);
  listCanvas.getTextBounds("X", 0, 0, &xbx, &xby, &xW, &xH);
  listCanvas.getTextBounds("]", 0, 0, &rbx, &rby, &rW, &rH);

  int16_t cbh = lH; if (xH > cbh) cbh = xH; if (rH > cbh) cbh = rH;

  int16_t checkTop = y + ((ROW_HEIGHT - cbh) / 2) + TEXT_Y_TWEAK + BRACKET_Y_TWEAK;
  int16_t baseL = checkTop - lby;
  int16_t baseR = checkTop - rby;

  int16_t checkX = PAD_X;
  int16_t xInsideStart = checkX + (int16_t)lW;
  int16_t xRightPos    = xInsideStart + ( (xW > (uint16_t)8 ? (int16_t)xW : 8) + (int16_t)BAR_EXPAND_X );
  bool    isSelected   = (taskIndex == selected);

  // Colors (with optional fade)
  uint16_t colBracket = colorForPriority(tasks[taskIndex].prio);
  uint16_t colMark    = isSelected ? COL_X_HARD : COL_FOOT;
  uint16_t colText    = tasks[taskIndex].done ? COL_DIM : COL_WHITE;  // ACTIVE TASKS = WHITE
  uint16_t colStrike  = COL_DIM;

  if (fadeAlpha >= 0) {
    uint8_t a = (uint8_t)fadeAlpha;
    colBracket = fade565(colBracket, fadeBg, a);
    colMark    = fade565(colMark,    fadeBg, a);
    colText    = fade565(colText,    fadeBg, a);
    colStrike  = fade565(colStrike,  fadeBg, a);
  }

  // Completed mark (inside [ ])
  if (tasks[taskIndex].done) {
    int16_t left   = xInsideStart;
    int16_t w      = xRightPos - xInsideStart;
    int16_t cx     = left + w/2;
    int16_t cy     = checkTop + cbh/2;

    auto drawThickLine = [&](int16_t x0,int16_t y0,int16_t x1,int16_t y1,uint16_t color,uint8_t thick){
      if (abs(y1-y0) >= abs(x1-x0)) {
        for (int8_t k=0; k<thick; ++k) listCanvas.drawLine(x0+k-thick/2,y0,x1+k-thick/2,y1,color);
      } else {
        for (int8_t k=0; k<thick; ++k) listCanvas.drawLine(x0,y0+k-thick/2,x1,y1+k-thick/2,color);
      }
    };

    switch (DONE_STYLE) {
      case DONE_BAR: {
        int16_t barH = BAR_THICKNESS;
        int16_t barY = cy - barH/2;
        int16_t barX = left  - (int16_t)BAR_UNDERLAP_L + (int16_t)BAR_MARGIN_X + BRACKET_X_BIAS;
        int16_t barW = w + (int16_t)BAR_UNDERLAP_L + (int16_t)BAR_UNDERLAP_R - 2*(int16_t)BAR_MARGIN_X;
        if (barW < 1) barW = 1;
        listCanvas.fillRect(barX, barY, barW, barH, colMark);
      } break;
      case DONE_X: {
        int16_t span = (w < cbh ? w : cbh) * 4 / 5; if (span < 6) span = (w < cbh ? w : cbh);
        int16_t lx = cx - span/2, rx = cx + span/2;
        int16_t top = cy - span/2, bot = cy + span/2;
        drawThickLine(lx, top, rx, bot, colMark, LINE_THICK);
        drawThickLine(lx, bot, rx, top, colMark, LINE_THICK);
      } break;
      case DONE_STAR: {
        int16_t span = (w < cbh ? w : cbh) * 4 / 5; if (span < 6) span = (w < cbh ? w : cbh);
        int16_t lx = cx - span/2, rx = cx + span/2;
        int16_t top = cy - span/2, bot = cy + span/2;
        drawThickLine(lx, cy, rx, cy, colMark, LINE_THICK);
        drawThickLine(cx, top, cx, bot, colMark, LINE_THICK);
        drawThickLine(lx, top, rx, bot, colMark, LINE_THICK);
        drawThickLine(lx, bot, rx, top, colMark, LINE_THICK);
      } break;
      case DONE_DOT: {
        listCanvas.fillCircle(cx, cy, DOT_RADIUS, colMark);
      } break;
    }
  }

  // Brackets
  listCanvas.setTextColor(colBracket);
  listCanvas.setCursor(checkX, baseL);    listCanvas.print("[");
  listCanvas.setCursor(xRightPos, baseR); listCanvas.print("]");

  // --- Text area ---
  const int16_t textX0 = PAD_X + (int16_t)lW + (xRightPos - xInsideStart) + (int16_t)rW + CHECK_GAP;
  int16_t availW = W - PAD_X - textX0; if (availW < 1) availW = 1;

  if (marqueeOffsetPx >= 0 && isSelected) {
    // --- NEW: compute pill mask height from bracket glyph to keep gaps equal ---
    int16_t pillH = cbh + 2 * (int16_t)HIL_GAP;
    int16_t rowCenterY = y + (int16_t)ROW_HEIGHT/2;
    int16_t pillY = rowCenterY - cbh/2 - (int16_t)HIL_GAP + TEXT_Y_TWEAK + BRACKET_Y_TWEAK;

    int16_t bx, by; uint16_t fullW, fullH;
    listCanvas.getTextBounds(tasks[taskIndex].text, 0, 0, &bx, &by, &fullW, &fullH);
    int16_t textYlocal = ((pillH - (int16_t)fullH) / 2) - by + TEXT_Y_TWEAK;

    GFXcanvas1 mc(availW, pillH);
    mc.fillScreen(0);
    mc.setTextWrap(false);
    mc.setTextSize(2);
    mc.setTextColor(1);

    int16_t x0 = -marqueeOffsetPx;
    mc.setCursor(x0, textYlocal); mc.print(tasks[taskIndex].text);
    mc.setCursor(x0 + (int16_t)fullW + (int16_t)MARQUEE_GAP_PX, textYlocal);
    mc.print(tasks[taskIndex].text);

    listCanvas.drawBitmap(textX0, pillY, mc.getBuffer(), mc.width(), mc.height(), colText);

    if (tasks[taskIndex].done) {
      int16_t mid = pillY + pillH/2;
      listCanvas.drawFastHLine(textX0, mid, availW, colStrike);
    }
  } else {
    // Normal (trim + ellipsis)
    char buf[24];
    strncpy(buf, tasks[taskIndex].text, sizeof(buf)-1);
    buf[sizeof(buf)-1] = '\0';

    int16_t bx, by; uint16_t bw, bh;
    listCanvas.getTextBounds(buf, 0, 0, &bx, &by, &bw, &bh);
    while (bw > (uint16_t)availW && strlen(buf) > 3) {
      size_t L = strlen(buf); buf[L-1] = '\0';
      listCanvas.getTextBounds(buf, 0, 0, &bx, &by, &bw, &bh);
    }
    if (strlen(buf) < strlen(tasks[taskIndex].text)) {
      size_t L = strlen(buf); if (L >= 3) { buf[L-3]='.'; buf[L-2]='.'; buf[L-1]='.'; }
      listCanvas.getTextBounds(buf, 0, 0, &bx, &by, &bw, &bh);
    }

    int16_t textY = y + ((ROW_HEIGHT - (int16_t)bh) / 2) - by + TEXT_Y_TWEAK;
    listCanvas.setCursor(textX0, textY);
    listCanvas.setTextColor(colText);
    listCanvas.print(buf);

    if (tasks[taskIndex].done) {
      int16_t x1,y1; uint16_t w2,h2;
      listCanvas.getTextBounds(buf, textX0, textY, &x1, &y1, &w2, &h2);
      int16_t mid = y1 + (int16_t)h2/2;
      listCanvas.drawFastHLine(textX0, mid, w2, colStrike);
    }
  }
}

// ---------- List compose/flush ----------
void composeListFrame(float hlRowY) {
  listCanvas.fillRect(0, 0, W, LIST_CANVAS_H, COL_BG);

  // --- Measure bracket glyph once to size the highlight pill ---
  listCanvas.setTextSize(2);
  int16_t lbx,lby, xbx,xby, rbx,rby;
  uint16_t lW,lH, xW,xH, rW,rH;
  listCanvas.getTextBounds("[", 0, 0, &lbx, &lby, &lW, &lH);
  listCanvas.getTextBounds("X", 0, 0, &xbx, &xby, &xW, &xH);
  listCanvas.getTextBounds("]", 0, 0, &rbx, &rby, &rW, &rH);
  int16_t cbh = lH; if (xH>cbh) cbh = xH; if (rH>cbh) cbh = rH;

  // Selected row center
  int16_t rowCenterY = (int16_t)(TOP_PAD + hlRowY + 0.5f);

  // Bracket top relative to row center + tweaks, then expand by HIL_GAP both ways
  int16_t brTop    = rowCenterY - cbh/2 + TEXT_Y_TWEAK + BRACKET_Y_TWEAK;
  int16_t brBottom = brTop + cbh;

  int16_t pillX = 4, pillW = W - 8;
  int16_t pillY = brTop - (int16_t)HIL_GAP;
  int16_t pillH = (brBottom - brTop) + 2*(int16_t)HIL_GAP;
  
  // Spacing between pill and bracket
  const int8_t HIL_Y_NUDGE = -1; // try 1 or -1 if needed
  pillY += HIL_Y_NUDGE;


  // Clamp to canvas
  if (pillY < 1) pillY = 1;
  if (pillY + pillH > LIST_CANVAS_H - 1) pillY = LIST_CANVAS_H - 1 - pillH;

  uint16_t pillCol = (mode == MODE_MOVE) ? COL_MOVE : COL_HIL;

  // --- Material-ish shadow behind the selection pill ---
  const int16_t shX = pillX + 1;
  const int16_t shY = pillY + 2;
  listCanvas.fillRoundRect(shX, shY, pillW, pillH, HIL_RADIUS + 1, COL_SHADOW);

  // Pill itself
  listCanvas.fillRoundRect(pillX, pillY, pillW, pillH, HIL_RADIUS, pillCol);

  // Draw border only outside MOVE mode
  if (HIL_BORDER_ENABLED && mode != MODE_MOVE) {
    uint16_t borderCol = HIL_BORDER_ACCENT ? COL_ACCENT : COL_BORDER;
    listCanvas.drawRoundRect(pillX, pillY, pillW, pillH, HIL_RADIUS, borderCol);
  }

  uint32_t now = millis();
  for (uint8_t i=0; i<VISIBLE_ROWS; ++i) {
    uint8_t idx = topIndex + i;
    int16_t rowY = TOP_PAD + i * ROW_HEIGHT;

    int fadeAlpha = -1;
    uint16_t fadeBg = (idx == selected) ? pillCol : COL_BG;

    if (idx < taskCount && tasks[idx].pend == PEND_FADE) {
      uint32_t elapsed = now - tasks[idx].animStartMs;
      if (elapsed >= FADE_ANIM_MS) fadeAlpha = 0;
      else {
        float t = (float)elapsed / (float)FADE_ANIM_MS;
        if (t < 0) t = 0; if (t > 1) t = 1;
        fadeAlpha = (uint8_t)(255.0f * (1.0f - t));
      }
    }

    int16_t mOffset = (g_marqueeActive && idx == selected) ? g_marqueeOffsetPx : -1;
    drawRowContentToCanvas(idx, rowY, fadeAlpha, fadeBg, mOffset);
  }

  uint16_t left  = (taskCount >= countDone()) ? (taskCount - countDone()) : 0;
  uint16_t total = taskCount;

  char nums[20];  snprintf(nums, sizeof(nums), "%u/%u", (unsigned)left, (unsigned)total);
  char full[28];  snprintf(full, sizeof(full), "[ %s ]", nums);

  listCanvas.setTextSize(2);
  int16_t fx,fy; uint16_t fw,fh;
  listCanvas.getTextBounds(full, 0, 0, &fx, &fy, &fw, &fh);

  int16_t lx,ly,nx,ny,rx,ry; uint16_t lw,lh,nw,nh,rw,rh;
  listCanvas.getTextBounds("[ ", 0, 0, &lx, &ly, &lw, &lh);
  listCanvas.getTextBounds(nums, 0, 0, &nx, &ny, &nw, &nh);
  listCanvas.getTextBounds(" ]", 0, 0, &rx, &ry, &rw, &rh);

  int16_t tx = W - PAD_X - (int16_t)fw + RIGHT_TWEAK_PX;
  int16_t ty = LIST_CANVAS_H - 2 - (int16_t)fh - fy;

  listCanvas.setTextColor(COL_WHITE); listCanvas.setCursor(tx, ty); listCanvas.print("[ ");
  listCanvas.setTextColor(COL_FOOT);  listCanvas.setCursor(tx + (int16_t)lw, ty); listCanvas.print(nums);
  listCanvas.setTextColor(COL_WHITE); listCanvas.setCursor(tx + (int16_t)lw + (int16_t)nw, ty); listCanvas.print(" ]");
}

inline void flushList() { tft.drawRGBBitmap(0, LIST_CANVAS_Y, listCanvas.getBuffer(), W, LIST_CANVAS_H); }

void rebuildListStatic() {
  float hlRowY = (selected - topIndex + 0.5f) * ROW_HEIGHT;
  composeListFrame(hlRowY);
  flushList();
}

// ---------- Anim ----------
void animateHighlight(int8_t dir) {
  float startY = (selected - topIndex + 0.5f - dir) * ROW_HEIGHT;
  float endY   = (selected - topIndex + 0.5f) * ROW_HEIGHT;
  for (uint8_t f=1; f<=ANIM_FRAMES; ++f) {
    float t = (float)f / ANIM_FRAMES;
    float y = startY + (endY - startY) * t;
    composeListFrame(y);
    flushList();
    delay(ANIM_DELAY_MS);
  }
}

// ---------- Movement & Helpers ----------
void ensureSelectionVisibleAndDraw(int8_t moveDir) {
  uint8_t oldTop = topIndex, oldSel = selected;
  if (moveDir > 0 && selected + 1 < taskCount) selected++;
  else if (moveDir < 0 && selected > 0)        selected--;
  if (selected >= topIndex + VISIBLE_ROWS) topIndex = selected - VISIBLE_ROWS + 1;
  if (selected < topIndex)                 topIndex = selected;

  if (selected != oldSel) {
    g_selStableSince = millis();
    g_marqueeActive  = false;
  }

  if (topIndex != oldTop || selected != oldSel) markUIDirty();
  if (topIndex != oldTop) rebuildListStatic();
  else if (selected != oldSel) animateHighlight((selected > oldSel) ? +1 : -1);
}

inline void swapTasks(uint8_t i, uint8_t j) { Task tmp = tasks[i]; tasks[i] = tasks[j]; tasks[j] = tmp; }

void moveSelectedRow(int8_t dir) {
  if (dir < 0) { if (selected == 0) return; swapTasks(selected, selected - 1); selected--; }
  else { if (selected + 1 >= taskCount) return; swapTasks(selected, selected + 1); selected++; }
  if (selected >= topIndex + VISIBLE_ROWS) topIndex = selected - VISIBLE_ROWS + 1;
  if (selected < topIndex)                 topIndex = selected;
  g_selStableSince = millis();
  g_marqueeActive  = false;
  markUIDirty();
  rebuildListStatic();
}

// ---- Delete helpers ----
int findIndexByUid(uint32_t uid) { for (uint8_t i=0;i<taskCount;i++) if (tasks[i].uid == uid) return i; return -1; }

void deleteByIndex(uint8_t idx) {
  if (idx >= taskCount) return;
  for (uint8_t i=idx; i<taskCount-1; ++i) tasks[i] = tasks[i+1];
  taskCount--;
  if (selected >= taskCount) selected = taskCount ? (taskCount - 1) : 0;
  if (selected > idx && selected>0) selected--;
  if (selected < topIndex) topIndex = selected;
  if (selected >= topIndex + VISIBLE_ROWS && taskCount >= VISIBLE_ROWS) topIndex = selected - VISIBLE_ROWS + 1;
  if (taskCount < VISIBLE_ROWS) topIndex = 0;
  saveTasksRuntime(); markUIDirty(); rebuildListStatic();
}

void deleteByUid(uint32_t uid) { int idx = findIndexByUid(uid); if (idx >= 0) deleteByIndex((uint8_t)idx); }

void performMoveToBottom(uint8_t idx) {
  if (idx >= taskCount) return;
  Task moved = tasks[idx];
  moved.pend = PEND_NONE; moved.pendStartMs = 0; moved.animStartMs = 0;
  for (uint8_t i = idx; i < taskCount - 1; ++i) tasks[i] = tasks[i + 1];
  tasks[taskCount - 1] = moved;
  if (selected > idx) selected--;
  if (selected < topIndex) topIndex = selected;
  if (selected >= topIndex + VISIBLE_ROWS) topIndex = selected - VISIBLE_ROWS + 1;
  saveTasksRuntime(); markUIDirty();
}

// Toggle by absolute index
void toggleByIndex(uint8_t idx) {
  if (idx >= taskCount) return;
  bool wasDone = tasks[idx].done;
  tasks[idx].done = !wasDone;
  if (!wasDone && tasks[idx].done) { tasks[idx].pend = PEND_WAIT; tasks[idx].pendStartMs = millis(); tasks[idx].animStartMs = 0; }
  else { tasks[idx].pend = PEND_NONE; tasks[idx].pendStartMs = 0; tasks[idx].animStartMs = 0; }
  saveTasksRuntime();
  g_selStableSince = millis();
  g_marqueeActive  = false;
  rebuildListStatic();
}
void toggleByUid(uint32_t uid) { if (uid == 0) return; for (uint8_t i=0; i<taskCount; ++i) if (tasks[i].uid == uid) { toggleByIndex(i); return; } }

// ---------- Priority change + sort ----------
void sortTasksByPriorityKeepSelection() {
  if (taskCount <= 1) return;
  uint32_t selUid = (selected < taskCount) ? tasks[selected].uid : 0;
  Task tmp[MAX_TASKS]; uint8_t n = 0;
  for (int pr = PRI_HIGH; pr >= PRI_LOW; --pr) for (uint8_t i=0; i<taskCount; ++i) if (!tasks[i].done && tasks[i].prio == (uint8_t)pr) tmp[n++] = tasks[i];
  for (uint8_t i=0; i<taskCount; ++i) if (tasks[i].done) tmp[n++] = tasks[i];
  for (uint8_t i=0; i<n; ++i) tasks[i] = tmp[i];
  if (selUid != 0) { int idx = findIndexByUid(selUid); if (idx >= 0) selected = (uint8_t)idx; }
  if (selected >= topIndex + VISIBLE_ROWS) topIndex = selected - VISIBLE_ROWS + 1;
  if (selected < topIndex) topIndex = selected;
}

void bumpPriorityAt(uint8_t idx, int8_t delta) {
  if (idx >= taskCount) return;
  int np = (int)tasks[idx].prio + (int)delta;
  if (np < PRI_LOW)  np = PRI_LOW;
  if (np > PRI_HIGH) np = PRI_HIGH;
  if (np == tasks[idx].prio) return;
  tasks[idx].prio = (uint8_t)np;
  saveTasksRuntime();
  sortTasksByPriorityKeepSelection();
  rebuildListStatic();
}

// ---------- Confirm modal ----------
void drawConfirmDelete() {
  const char* title = "Delete?";
  const char* optL  = "Y (UP)";
  const char* optR  = "N (DOWN)";

  tft.setTextWrap(false);
  tft.setTextSize(2);

  int16_t x1,y1; uint16_t wTitle,hTitle;
  tft.getTextBounds(title, 0, 0, &x1, &y1, &wTitle, &hTitle);

  int16_t xa,ya,xb,yb; uint16_t wL,hL,wR,hR;
  tft.getTextBounds(optL, 0, 0, &xa, &ya, &wL, &hL);
  tft.getTextBounds(optR, 0, 0, &xb, &yb, &wR, &hR);

  const int16_t padX = 12, padY = 14, gapY = 12; int16_t gap = 12;

  int16_t optsRowW = (int16_t)wL + gap + (int16_t)wR;
  int16_t contentW = ((int16_t)wTitle > optsRowW) ? (int16_t)wTitle : optsRowW;
  int16_t bw = min((int16_t)(W - 2*8), (int16_t)(contentW + padX*2));
  int16_t innerW = bw - padX*2;
  if (optsRowW > innerW) { int16_t extra = optsRowW - innerW; int16_t newGap = gap - extra; if (newGap < 6) newGap = 6; gap = newGap; }
  int16_t bh = (int16_t)hTitle + gapY + max(hL, hR) + padY*2;

  int16_t bx = (W - bw) / 2;
  int16_t by = HEADER_HEIGHT + (LIST_CANVAS_H - bh) / 2;

  // Shadow behind modal panel
  tft.fillRoundRect(bx + 2, by + 3, bw, bh, 12, COL_SHADOW);

  uint16_t COL_PANEL = COL_MODAL_PANEL, COL_TXT = COL_BLACK;

  tft.fillRoundRect(bx, by, bw, bh, 10, COL_PANEL);
  tft.drawRoundRect(bx, by, bw, bh, 10, COL_PANEL);

  tft.setTextColor(COL_TXT, COL_PANEL);
  int16_t tx = bx + (bw - (int16_t)wTitle)/2;
  int16_t ty = by + padY - y1;
  tft.setCursor(tx, ty); tft.print(title);

  int16_t rowX = bx + (bw - ((int16_t)wL + gap + (int16_t)wR))/2;
  int16_t rowY = ty + (int16_t)hTitle + gapY - ya;

  tft.setCursor(rowX, rowY); tft.print(optL);

  int16_t rightX = rowX + (int16_t)wL + gap;
  tft.setCursor(rightX, rowY - (yb - ya)); tft.print(optR);
}

// ---------- Read (full-task) modal ----------
static void wrapTextToLines(const char* s, char out[][64], uint8_t &lines, uint8_t maxLines, uint8_t maxCharsPerLine) {
  lines = 0; if (!s || !*s) return;
  size_t N = strlen(s);
  char work[160];
  size_t M = (N < sizeof(work)-1) ? N : (sizeof(work)-1);
  memcpy(work, s, M); work[M]='\0';

  size_t i = 0;
  while (i < M && lines < maxLines) {
    while (i < M && work[i]==' ') i++; if (i >= M) break;
    size_t start = i, len = 0, lastSpace = (size_t)-1;
    while (i < M) {
      if (work[i] == ' ') lastSpace = i;
      if (len >= maxCharsPerLine) break;
      i++; len++;
      if (len >= maxCharsPerLine) break;
    }
    size_t end;
    if (i < M && work[i] != ' ' && lastSpace != (size_t)-1 && lastSpace >= start) { end = lastSpace; i = lastSpace + 1; }
    else { end = i; }
    size_t L = end - start; if (L > 63) L = 63;
    memcpy(out[lines], &work[start], L); out[lines][L] = '\0';
    lines++;
  }
}

void drawReadModal(uint8_t idx) {
  if (idx >= taskCount) return;

  tft.setTextWrap(false);
  tft.setTextSize(2);

  int16_t bxm,bym; uint16_t xW,xH;
  tft.getTextBounds("W", 0, 0, &bxm, &bym, &xW, &xH); if (xW == 0) xW = 8;

  const int16_t padX = 12, padY = 14;
  int16_t bw = W - 16; int16_t innerW = bw - padX*2;
  uint8_t maxCharsPerLine = innerW / (int16_t)xW; if (maxCharsPerLine < 6) maxCharsPerLine = 6;

  char lines[5][64]; uint8_t nLines = 0;
  wrapTextToLines(tasks[idx].text, lines, nLines, 5, maxCharsPerLine);
  if (nLines == 0) { strcpy(lines[0], "(empty)"); nLines = 1; }

  int16_t txb, tyb; uint16_t tw, th;
  tft.getTextBounds("X", 0, 0, &txb, &tyb, &tw, &th);
  int16_t textH = (int16_t)nLines * (int16_t)th;

  int16_t bh = padY*2 + textH; if (bh > LIST_CANVAS_H - 12) bh = LIST_CANVAS_H - 12;

  int16_t bx = (W - bw) / 2;
  int16_t by = HEADER_HEIGHT + (LIST_CANVAS_H - bh) / 2;

  // Shadow behind modal panel
  tft.fillRoundRect(bx + 2, by + 3, bw, bh, 12, COL_SHADOW);

  uint16_t COL_PANEL = COL_HIL, COL_TXT = COL_BLACK;

  tft.fillRoundRect(bx, by, bw, bh, 10, COL_PANEL);
  tft.drawRoundRect(bx, by, bw, bh, 10, COL_PANEL);

  int16_t ty = by + padY - tyb;
  int16_t tx = bx + padX;

  tft.setTextColor(COL_TXT, COL_PANEL);
  for (uint8_t i=0; i<nLines; ++i) { tft.setCursor(tx, ty + (int16_t)i * (int16_t)th); tft.print(lines[i]); }
}

// ---------- Mode transitions ----------
static inline void drainAllButtons() { Btn::drain(bUp); Btn::drain(bDown); Btn::drain(bOk); }

void enterMoveMode() { mode = MODE_MOVE; g_marqueeActive = false; markUIDirty(); rebuildHeader(true); rebuildListStatic(); }

void exitMoveMode(bool persist=true) {
  mode = MODE_NORMAL;
  if (persist) saveTasksRuntime();
  g_selStableSince = millis();
  g_marqueeActive  = false;
  markUIDirty(); rebuildHeader(true); rebuildListStatic();
}

void enterConfirmDelete(uint32_t uid) {
  mode = MODE_CONFIRM_DELETE; confirmDeleteUid = uid;
  g_okClicks = 0; g_okAnchorUid = 0; g_okLastClickMs = 0;
  g_marqueeActive = false;
  drainAllButtons(); confirmIgnoreUntil = millis() + 200;
  rebuildHeader(true); drawConfirmDelete();
}

void exitConfirmDelete(bool confirmed) {
  if (confirmed && confirmDeleteUid) deleteByUid(confirmDeleteUid);
  confirmDeleteUid = 0; mode = MODE_NORMAL;
  drainAllButtons(); inputSquelchUntil = millis() + 180;
  g_okClicks = 0; g_okAnchorUid = 0; g_okLastClickMs = 0;
  g_selStableSince = millis();
  g_marqueeActive  = false;
  markUIDirty(); rebuildHeader(true); rebuildListStatic();
}

void enterReadModal() { mode = MODE_READ; g_marqueeActive = false; g_okClicks = 0; g_okAnchorUid = 0; rebuildHeader(true); drawReadModal(selected); }
void exitReadModal()  { mode = MODE_NORMAL; drainAllButtons(); inputSquelchUntil = millis() + 180; g_selStableSince = millis(); g_marqueeActive = false; rebuildHeader(true); rebuildListStatic(); }

// --- Priority edit transitions ---
void enterPrioEdit(int8_t dir /*+1 up, -1 down*/) {
  if (taskCount == 0) return;
  mode = MODE_PRIO_EDIT;
  prioEditDir = dir;
  g_marqueeActive = false;
  // Apply one bump immediately on entry
  bumpPriorityAt(selected, prioEditDir);
  // Schedule the next bump slowly
  g_prioNextBumpMs = millis() + PRIO_REPEAT_MS;
  rebuildHeader(true);
}

void exitPrioEdit() {
  mode = MODE_NORMAL;
  prioEditDir = 0;
  rebuildHeader(true);
  rebuildListStatic();
}

// ---------- USB Serial helpers ----------
static uint8_t prioFromChar(char c) {
  if (c=='H'||c=='h') return PRI_HIGH;
  if (c=='M'||c=='m') return PRI_MED;
  return PRI_LOW;
}
static char prioToChar(uint8_t p) {
  if (p==PRI_HIGH) return 'H';
  if (p==PRI_MED)  return 'M';
  return 'L';
}
void serialPrintHelp() {
  Serial.println(F("Commands:"));
  Serial.println(F("  HELP"));
  Serial.println(F("  LIST"));
  Serial.println(F("  EXPORT"));
  Serial.println(F("  IMPORT   (then lines: TASK|H|0|Your text ; finish with END)"));
  Serial.println(F("  ADD <H|M|L> <text...>"));
  Serial.println(F("  CLEAR"));
}
void serialList() {
  Serial.print(F("Count: ")); Serial.println(taskCount);
  for (uint8_t i=0;i<taskCount;i++) {
    Serial.print(i); Serial.print(F(": "));
    Serial.print(prioToChar(tasks[i].prio)); Serial.print(F(" | "));
    Serial.print(tasks[i].done ? '1':'0');   Serial.print(F(" | "));
    Serial.println(tasks[i].text);
  }
}
void serialExport() {
  Serial.println(F("BEGIN"));
  for (uint8_t i=0;i<taskCount;i++) {
    Serial.print(F("TASK|"));
    Serial.print(prioToChar(tasks[i].prio));
    Serial.print(F("|"));
    Serial.print(tasks[i].done ? '1':'0');
    Serial.print(F("|"));
    Serial.println(tasks[i].text);
  }
  Serial.println(F("END"));
}
void serialClear() {
  taskCount = 0;
  saveTasksRuntime();
  rebuildListStatic();
  Serial.println(F("OK CLEARED"));
}
void serialAdd(uint8_t pr, const char* text, bool done=false) {
  if (taskCount >= MAX_TASKS) { Serial.println(F("ERR FULL")); return; }
  Task t{};
  strncpy(t.text, text, sizeof(t.text)-1);
  t.text[sizeof(t.text)-1] = '\0';
  t.done = done;
  t.prio = pr;
  t.pend = PEND_NONE;
  t.pendStartMs = 0;
  t.animStartMs = 0;
  t.uid = nextUid++;
  tasks[taskCount++] = t;
  saveTasksRuntime();
  sortTasksByPriorityKeepSelection();
  rebuildListStatic();
  Serial.println(F("OK ADDED"));
}
void serialParseLine(char* line) {
  // Trim CR/LF
  size_t n = strlen(line);
  while (n && (line[n-1]=='\r' || line[n-1]=='\n')) { line[n-1]=0; n--; }
  if (!n) return;

  if (g_serialImport) {
    if (strcmp(line, "END")==0) {
      g_serialImport = false;
      saveTasksRuntime();
      sortTasksByPriorityKeepSelection();
      rebuildListStatic();
      Serial.println(F("OK IMPORT END"));
      return;
    }
    // Expect: TASK|H|0|text...
    if (strncmp(line, "TASK|", 5)==0) {
      char* p = line + 5;
      if (!*p) { Serial.println(F("ERR FORMAT")); return; }
      uint8_t pr = prioFromChar(*p); p++;
      if (*p!='|') { Serial.println(F("ERR FORMAT")); return; }
      p++;
      if (!*p) { Serial.println(F("ERR FORMAT")); return; }
      bool done = (*p=='1'); // '1' = done
      while (*p && *p!='|') p++;
      if (*p!='|') { Serial.println(F("ERR FORMAT")); return; }
      p++;
      // p now points to text (can be empty)
      serialAdd(pr, p, done);
      return;
    } else {
      Serial.println(F("ERR EXPECT TASK|H|0|text or END"));
      return;
    }
  }

  // Normal command mode
  if (strcmp(line,"HELP")==0) { serialPrintHelp(); return; }
  if (strcmp(line,"LIST")==0) { serialList(); return; }
  if (strcmp(line,"EXPORT")==0) { serialExport(); return; }
  if (strcmp(line,"IMPORT")==0) { g_serialImport=true; Serial.println(F("OK IMPORT BEGIN")); return; }
  if (strcmp(line,"CLEAR")==0) { serialClear(); return; }

  if (strncmp(line,"ADD ",4)==0) {
    char* p = line + 4;
    if (!*p) { Serial.println(F("ERR ADD")); return; }
    uint8_t pr = PRI_LOW;
    if (*p=='H'||*p=='h'||*p=='M'||*p=='m'||*p=='L'||*p=='l') {
      pr = prioFromChar(*p);
      p++;
      if (*p==' ') p++;
    }
    if (!*p) { Serial.println(F("ERR ADD TEXT")); return; }
    serialAdd(pr, p, false);
    return;
  }

  Serial.println(F("ERR UNKNOWN. Type HELP."));
}

void serialService() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c=='\n' || c=='\r') {
      g_lineBuf[g_lineLen]=0;
      serialParseLine(g_lineBuf);
      g_lineLen = 0;
    } else {
      if (g_lineLen < sizeof(g_lineBuf)-1) g_lineBuf[g_lineLen++] = c;
    }
  }
}

// ---------- Arduino ----------
void setup() {
#if USE_HARDWARE_SPI
  #if defined(ESP32)
    SPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);
  #else
    SPI.begin(); // Teensy/others: hardware pins fixed
  #endif
#endif
  tft.init(135, 240);
  tft.setRotation(3);
#if USE_HARDWARE_SPI
  tft.setSPISpeed(SPI_SPEED_HZ);
#endif

  // Colors — Catppuccin Frappé (tuned for contrast on ST7789)
  COL_BG          = tft.color565(0x30, 0x34, 0x46);  // Base        #303446
  COL_TEXT        = tft.color565(0xC6, 0xD0, 0xF5);  // Text        #C6D0F5 (kept for other UI)

  // Darker "dim" so strikes and completed text are clearly lower-contrast
  COL_DIM         = tft.color565(0x83, 0x8B, 0xA7);  // Overlay1    #838BA7

  // Accents / structure
  COL_ACCENT      = tft.color565(0x85, 0xC1, 0xDC);  // Sapphire    #85C1DC
  COL_FRAME       = tft.color565(0x51, 0x57, 0x6D);  // Surface1    #51576D
  COL_BORDER      = tft.color565(0x73, 0x79, 0x94);  // Overlay0    #737994

  // Selection pill — more saturated than Lavender for pop
  COL_HIL         = tft.color565(0x8C, 0xAA, 0xEE);  // Blue        #8CAAEE

  // Footer numbers / light marks — slightly brighter than Teal for readability
  COL_FOOT        = tft.color565(0x99, 0xD1, 0xDB);  // Sky         #99D1DB

  COL_BLACK       = tft.color565(0x00, 0x00, 0x00);  // Black
  COL_MODAL_PANEL = tft.color565(0xF2, 0xD5, 0xCF);  // Rosewater   #F2D5CF

  // MOVE color updated to #EED49F
  COL_MOVE        = tft.color565(0xEE, 0xD4, 0x9F);  // Yellow-ish  #EED49F

  // True white for task text
  COL_WHITE       = tft.color565(0xFF, 0xFF, 0xFF);  // pure white

  COL_X_HARD      = tft.color565(0xA6, 0xD1, 0x89);  // Green       #A6D189 (done mark)

  // Priority colors
  COL_PRI_HIGH    = tft.color565(0xE7, 0x82, 0x84);  // Red (HIGH)  #E78284
  COL_PRI_MED     = tft.color565(0xEF, 0x9F, 0x76);  // Peach (MED) #EF9F76

  // Clock — keep your current Sky, or use Frappé Sky below
  COL_CLOCK       = tft.color565(0x89, 0xDC, 0xEB);  // current     #89DCEB
  // COL_CLOCK    = tft.color565(0x99, 0xD1, 0xDB);  // Frappé Sky  #99D1DB

  tft.fillScreen(COL_BG);

  // Soft shadow for Material-like elevation (black over base at ~30% strength)
  COL_SHADOW = fade565(COL_BLACK, COL_BG, 80); // 0..255 (higher = darker). Try 70–110 to taste.

  Btn::init(bUp); Btn::init(bOk); Btn::init(bDown);

  Serial.begin(SERIAL_BAUD);

  applySeedIfChangedAndLoad();
  loadUIState();
  mode = MODE_NORMAL;
  sortTasksByPriorityKeepSelection();

  clockStartMs = millis();
  g_selStableSince = clockStartMs;
  g_marqueeActive  = false;

  rebuildHeader(true);
  rebuildListStatic();
}

void loop() {
  uint32_t now = millis();

  // USB Serial service (non-blocking)
  serialService();

  // Update buttons (events computed per frame)
  Btn::update(bUp); Btn::update(bDown); Btn::update(bOk);

  bool inputsReady = ((int32_t)(now - inputSquelchUntil) >= 0);

  // Long-hold OK to read (only from NORMAL)
  if (mode == MODE_NORMAL && inputsReady && Btn::heldFor(bOk, READ_HOLD_MS)) {
    if (taskCount > 0) enterReadModal();
  }

  // ======== MODE handlers ========
  if (mode == MODE_MOVE && inputsReady) {
    if (Btn::pressed(bUp) || Btn::repeat(bUp))     moveSelectedRow(-1);
    if (Btn::pressed(bDown) || Btn::repeat(bDown)) moveSelectedRow(+1);

  } else if (mode == MODE_CONFIRM_DELETE) {
    bool gateOpen = ( (int32_t)(now - confirmIgnoreUntil) >= 0 )
                    && !bUp.debPressed && !bDown.debPressed && !bOk.debPressed;
    if (gateOpen) {
      if (Btn::clicked(bUp))   { exitConfirmDelete(true); }
      if (Btn::clicked(bDown)) { exitConfirmDelete(false); }
    }

  } else if (mode == MODE_READ) {
    // close read modal on release of OK
    if (!bOk.debPressed && Btn::released(bOk)) exitReadModal();

  } else if (mode == MODE_PRIO_EDIT) {
    // SLOW priority cycling: ignore Btn::repeat and use our own timer
    if (prioEditDir > 0) {
      if (!bUp.debPressed) { exitPrioEdit(); }
      else if ((int32_t)(now - g_prioNextBumpMs) >= 0) {
        bumpPriorityAt(selected, +1);
        g_prioNextBumpMs = now + PRIO_REPEAT_MS;
      }
    } else if (prioEditDir < 0) {
      if (!bDown.debPressed) { exitPrioEdit(); }
      else if ((int32_t)(now - g_prioNextBumpMs) >= 0) {
        bumpPriorityAt(selected, -1);
        g_prioNextBumpMs = now + PRIO_REPEAT_MS;
      }
    }

  } else if (mode == MODE_NORMAL && inputsReady) {
    // --- Hold to edit priority (no initial scroll); short tap to move on release ---
    bool enteredPrio = false;

    // Hold starts FIRST → enter priority editor immediately
    if (Btn::holdStart(bUp))        { enterPrioEdit(+1); enteredPrio = true; }
    else if (Btn::holdStart(bDown)) { enterPrioEdit(-1); enteredPrio = true; }

    if (!enteredPrio) {
      // Short tap navigation: move on release if press duration < HOLD_START_MS
      if (Btn::released(bUp) && Btn::lastPressDurationMs(bUp) < HOLD_START_MS) {
        ensureSelectionVisibleAndDraw(-1);
      }
      if (Btn::released(bDown) && Btn::lastPressDurationMs(bDown) < HOLD_START_MS) {
        ensureSelectionVisibleAndDraw(+1);
      }
    }
  }

  // ---- OK multi-clicks (not in confirm/read/prio) ----
  if (mode != MODE_CONFIRM_DELETE && mode != MODE_READ && mode != MODE_PRIO_EDIT && inputsReady && Btn::clicked(bOk)) {
    if (g_okClicks == 0) {
      g_okClicks = 1; g_okLastClickMs = now;
      g_okAnchorUid = (selected < taskCount) ? tasks[selected].uid : 0; // anchor the row
    } else if (now - g_okLastClickMs <= MULTI_MS) {
      g_okClicks++; g_okLastClickMs = now;
      if (g_okClicks >= 3) {
        enterConfirmDelete(g_okAnchorUid);
        g_okClicks = 0;
        g_okAnchorUid = 0;
      }
    } else {
      if (mode != MODE_MOVE && g_okAnchorUid) toggleByUid(g_okAnchorUid); // SAFETY: no toggle in MOVE
      g_okClicks = 1; g_okLastClickMs = now;
      g_okAnchorUid = (selected < taskCount) ? tasks[selected].uid : 0;
    }
  }

  // Resolve single/double after window expires
  if (mode != MODE_CONFIRM_DELETE && mode != MODE_READ && mode != MODE_PRIO_EDIT && g_okClicks > 0 && (now - g_okLastClickMs > MULTI_MS)) {
    uint8_t n = g_okClicks; g_okClicks = 0;

    if (mode == MODE_MOVE) {
      if (n == 2) exitMoveMode(true);  // single ignored in MOVE
    } else {
      if (n == 1) {
        if (g_okAnchorUid) toggleByUid(g_okAnchorUid);
      } else if (n == 2) {
        if (mode == MODE_NORMAL) enterMoveMode();
        else                     exitMoveMode(true);
      }
    }
    g_okAnchorUid = 0;
  }

  // ---- Per-task pending pipeline: WAIT -> FADE -> move-to-bottom ----
  bool changed = false;
  bool anyFading = false;

  for (uint8_t i = 0; i < taskCount; ) {
    if (tasks[i].pend == PEND_WAIT) {
      if ((int32_t)(now - tasks[i].pendStartMs) >= (int32_t)PENDING_DELAY_MS) {
        tasks[i].pend = PEND_FADE;
        tasks[i].animStartMs = now;
        changed = true;
      }
      i++;
    } else if (tasks[i].pend == PEND_FADE) {
      uint32_t elapsed = now - tasks[i].animStartMs;
      if (elapsed >= FADE_ANIM_MS) {
        performMoveToBottom(i);
        changed = true;
      } else {
        anyFading = true;
        i++;
      }
    } else {
      i++;
    }
  }

  if ((changed || anyFading) && mode != MODE_READ) {
    rebuildListStatic();  // continuous redraw during fades + after moves
  }

  // ===== Auto-scroll (marquee) scheduler =====
  if (mode == MODE_NORMAL && taskCount > 0) {
    int16_t textX, availW; computeTextArea(textX, availW);
    int16_t bx, by; uint16_t tw, th;
    listCanvas.setTextSize(2);
    listCanvas.getTextBounds(tasks[selected].text, 0, 0, &bx, &by, &tw, &th);

    bool tooLong = (tw > (uint16_t)availW);
    if (tooLong) {
      if (!g_marqueeActive) {
        if ((uint32_t)(now - g_selStableSince) >= MARQUEE_IDLE_MS) {
          g_marqueeActive = true;
          g_marqueeStartMs = now;
          g_lastMarqueeFrame = 0;
        }
      }
      if (g_marqueeActive) {
        uint32_t elapsed = now - g_marqueeStartMs;
        uint32_t cycleW  = (uint32_t)tw + (uint32_t)MARQUEE_GAP_PX;
        uint32_t px      = (elapsed * MARQUEE_SPEED_PX_PER_S) / 1000U;
        if (cycleW == 0) cycleW = 1;
        g_marqueeOffsetPx = (int16_t)(px % cycleW);

        if ((uint32_t)(now - g_lastMarqueeFrame) >= MARQUEE_FRAME_MS) {
          rebuildListStatic();
          g_lastMarqueeFrame = now;
        }
      }
    } else {
      g_marqueeActive = false;
    }
  } else {
    g_marqueeActive = false;
  }

  if (uiDirty && (now - lastUISaveMs) >= UI_SAVE_DEBOUNCE_MS) saveUIState();

  rebuildHeader(false);
}
