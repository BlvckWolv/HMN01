// ========== My Day (Nano ESP32 / ST7789 320x170 + 5-way stick + NAV Home + PWM Backlight) ==========
// Buttons to GND (internal pullups): A4=UP, A3=OK, A2=DOWN, A5=LEFT, A6=RIGHT
// OK: single toggle, double enter/exit MOVE, triple delete->confirm
//
// Display: ST7789 1.9" 170x320, rotation=3, offset default (35,0) — change to (0,35) if your panel needs that.
//
// This build:
// - Left navigation "NAV Home" PNG (alpha) with 2px padding all around
// - MOVE mode: UP/DOWN moves rows, LEFT/RIGHT adjusts brightness (OSD shows %; saved to Preferences)
// - Keeps marquee, read modal, serial import/export, etc.
// - BATTERY OVERLAY REMOVED, and backlight pin set to D7.

#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <math.h>
#include <pgmspace.h>

#include "BG_COL.h"      // BG_COLOR565 from your PNG color

// NAV_HOME removed: we now render a simple solid-color vertical navigation bar on the left

// ----------- Core gating -----------
#if defined(ESP32) || defined(ARDUINO_ARCH_ESP32)
  #define MYDAY_ESP32 1
#else
  #define MYDAY_ESP32 0
#endif

#if MYDAY_ESP32
  #include <Preferences.h>
  #ifdef __has_include
    #if __has_include("esp32-hal-ledc.h")
      #include "esp32-hal-ledc.h"
    #endif
  #endif
  Preferences prefs;
#else
  // Minimal stub for non-ESP32
  class Preferences {
   public:
    void begin(const char*, bool) {}
    void end() {}
    void clear() {}
    uint8_t  getUChar(const char*, uint8_t def=0){return def;}
    void     putUChar(const char*, uint8_t){}
    uint32_t getUInt(const char*, uint32_t def=0){return def;}
    void     putUInt(const char*, uint32_t){}
    String   getString(const char*, const String& def=""){return def;}
    void     putString(const char*, const String&) {}
  } prefs;
#endif

#include <string.h>


// ---------- Display / Pins ----------
//
// ======= USER CONFIG (tweak here) =======
// TFT pins should match your wiring (Nano ESP32 example below).
// Backlight:
//   - TFT_BL: set to your BL gate pin (you said D7)
//   - BL_INVERT: 0 for non-inverting (direct LED or low-side NPN that brightens with higher duty)
//                1 if your hardware acts inverted (higher duty = dimmer)
//   - BL_FREQ:   1000 Hz works well with PN2222/BJTs; you can try 2000–4000 Hz too
//   - BL_MIN/MAX_PCT: UI range (5..90 by request)
//   - BL_GAMMA:  perceptual smoothing (2.2 is a good default; lower = more linear)
//   - BL_USER_MIN/MAX: clamp the effective floor/ceiling to avoid full-off or overdrive
// ========================================

#define USE_HARDWARE_SPI 1
#define SPI_SPEED_HZ     40000000  // 40 MHz

// Nano ESP32 pins you wired:
#define TFT_CS    9
#define TFT_DC    10
#define TFT_RST   11
#define TFT_MOSI  12
#define TFT_SCLK  13

// Backlight PWM pin (your MOSFET gate → BLK). User wired BL to D7.
#define TFT_BL    7   // D7

// ---------- Backlight / brightness ----------
#if MYDAY_ESP32
  static const int BL_FREQ =  1000;  //  1 kHz (better for PN2222/BJT paths)
  static const int BL_BITS = 12;     // 0..4095
#endif

// If MOSFET path inverts (common: higher duty = dim), keep 1. If direct-LED PWM, set 0.
#define BL_INVERT 1

static const uint8_t BL_MIN_PCT = 5;
static const uint8_t BL_MAX_PCT = 90;
static const uint8_t BL_STEP    = 4;
static uint8_t  g_blPct         = 60;
static const float   BL_GAMMA    = 2.2f;   // perceptual smoothing
static const float   BL_USER_MIN = 0.05f;  // 5% floor (avoids off)
static const float   BL_USER_MAX = 0.90f;  // 90% ceiling
static uint32_t g_blOsdUntil    = 0;
static bool     g_blDirty       = false;
static uint32_t g_blLastSaveMs  = 0;

static inline uint16_t pctToDuty(uint8_t pct) {
#if MYDAY_ESP32
  if (pct < BL_MIN_PCT) pct = BL_MIN_PCT;
  if (pct > BL_MAX_PCT) pct = BL_MAX_PCT;
  const uint16_t full = (1u << BL_BITS) - 1; // e.g., 4095 for 12-bit

  // Map UI percent (5..90) to perceptual brightness (0.05..0.90) with gamma
  float t = float(pct - BL_MIN_PCT) / float(BL_MAX_PCT - BL_MIN_PCT); // 0..1
  if (t < 0.0f) t = 0.0f; if (t > 1.0f) t = 1.0f;
  float pg = powf(t, BL_GAMMA);                    // perceptual curve
  float b  = BL_USER_MIN + pg * (BL_USER_MAX - BL_USER_MIN); // 0.05..0.90

  // Convert brightness fraction to duty
  // Non-inverting path: duty = b * full
  // Inverting path (common with MOSFET): higher duty = dimmer -> duty = (1 - b) * full
  float df = BL_INVERT ? (1.0f - b) : b;
  if (df < 0.0f) df = 0.0f; if (df > 1.0f) df = 1.0f;
  uint32_t duty = (uint32_t)(df * float(full) + 0.5f);

  return (uint16_t)duty;
#else
  return 0;
#endif
}

static inline void applyBacklight() {
#if MYDAY_ESP32
  ledcWrite(TFT_BL, pctToDuty(g_blPct));
#endif
}

static inline void setupBacklightPwm() {
#if MYDAY_ESP32
  ledcAttach(TFT_BL, BL_FREQ, BL_BITS);
  applyBacklight();
#endif
}

static inline void setBacklightPct(uint8_t pct, bool showOsd=true) {
  if (pct < BL_MIN_PCT) pct = BL_MIN_PCT;
  if (pct > BL_MAX_PCT) pct = BL_MAX_PCT;
  g_blPct = pct;
  applyBacklight();
  if (showOsd) g_blOsdUntil = millis() + 1500;
  g_blDirty = true;
  g_blLastSaveMs = millis();
}

// ---------- Build & seed ----------
#define TASK_TEXT_MAX 96
#define APPLY_SEED_ON_NEW_BUILD 1
static const char* BUILD_ID = __DATE__ " " __TIME__;

// ---------- Layout (320x170) ----------
const int16_t W = 320, H = 170;
const uint8_t HEADER_HEIGHT = 22;
const uint8_t HEADER_Y_PAD  = 4;
const uint8_t ROW_HEIGHT    = 26;
const uint8_t VISIBLE_ROWS  = 5;
const int16_t LIST_CANVAS_Y = HEADER_HEIGHT;
const int16_t LIST_CANVAS_H = H - HEADER_HEIGHT;
const uint8_t TOP_PAD       = 2;
const uint8_t FOOTER_BOTTOM_PAD = 0;

#define ENABLE_PILL_PULSE 0

// ---- Left navigation geometry ----
static const int16_t NAV_PAD = 2; // 2px on all sides
static const int16_t NAV_WIDTH = 32; // panel width (32x170)
static const int16_t NAV_HEIGHT = H - 2 * NAV_PAD; // height within top/bottom padding
static const int16_t NAV_RADIUS = 6; // less rounded corners
static inline int16_t MAIN_LEFT() { return (int16_t)NAV_WIDTH + NAV_PAD + 3; }

// ---------- Buttons ----------
#define BTN_UP    A4
#define BTN_OK    A3
#define BTN_DOWN  A2
#define BTN_LEFT  A5
#define BTN_RIGHT A6

const uint16_t DEBOUNCE_MS = 12;
const uint16_t MIN_PRESS_MS = 20;
const uint16_t MIN_RELEASE_MS= 30;
const uint16_t MULTI_MS = 300;
const uint16_t HOLD_START_MS = 220;
const uint16_t REPEAT_PERIOD_S = 70;
const uint16_t REPEAT_PERIOD_F = 24;
const uint32_t REPEAT_ACCEL1 = 400;
const uint32_t REPEAT_ACCEL2 = 800;
const uint16_t PRIO_REPEAT_MS = 200;
const uint8_t  ANIM_FRAMES = 4;
const uint8_t  ANIM_DELAY_MS = 8;
const uint8_t  HIL_RADIUS = 3;
const bool     HIL_BORDER_ENABLED = true;
const bool     HIL_BORDER_ACCENT  = true;
const uint8_t  HIL_GAP = 2;
const int8_t   TEXT_Y_TWEAK = 1;
const int8_t   BRACKET_Y_TWEAK = 1;
const int8_t   BRACKET_X_BIAS = 0;
const int8_t   TASK_TEXT_X_TWEAK = -3;
const int8_t   BRACKET_X_TWEAK   = 0;

enum DoneStyle { DONE_BAR=0, DONE_X, DONE_STAR, DONE_DOT };
const DoneStyle DONE_STYLE = DONE_STAR;

const uint8_t LINE_THICK = 2;
const uint8_t DOT_RADIUS = 5;
const uint8_t BAR_MARGIN_X = 0;
const uint8_t BAR_THICKNESS = 5;
const uint8_t BAR_UNDERLAP_L= 4;
const uint8_t BAR_UNDERLAP_R= 4;
const uint8_t BAR_EXPAND_X = 5;
const int8_t  RIGHT_TWEAK_PX = 4;

// ---------- Colors ----------
uint16_t COL_BG, COL_TEXT, COL_DIM, COL_ACCENT, COL_FRAME, COL_BORDER;
uint16_t COL_FOOT, COL_BLACK, COL_MODAL_PANEL, COL_MOVE, COL_HIL, COL_WHITE, COL_X_HARD;
uint16_t COL_PRI_HIGH, COL_PRI_MED, COL_CLOCK, COL_SHADOW;
uint16_t COL_NAV_BAR;
uint16_t COL_NAV_ICON0, COL_NAV_ICON1, COL_NAV_ICON2, COL_NAV_ICON3, COL_ICON_GREY;
uint16_t COL_BOLT;
uint16_t COL_TAB_HIL; // #a6d189
uint16_t COL_NAV_SEL; // indicator bar
uint16_t COL_DATE_TXT; // #b8c0e0
uint16_t COL_SAVER_PILL; // #f5a97f

// ---------- Tasks & Persistence ----------
enum PendState  : uint8_t { PEND_NONE=0, PEND_WAIT=1, PEND_FADE=2 };
enum Priority   : uint8_t { PRI_LOW=0, PRI_MED=1, PRI_HIGH=2 };

struct Task {
  char     text[TASK_TEXT_MAX];
  bool     done;
  uint8_t  prio;
  uint8_t  pend;
  uint32_t pendStartMs;
  uint32_t animStartMs;
  uint32_t uid;
};

const uint8_t MAX_TASKS = 30;
Task     tasks[MAX_TASKS];
uint8_t  taskCount = 0;
uint32_t nextUid = 1;

// ---------- Modes ----------
enum Mode : uint8_t { MODE_NORMAL=0, MODE_MOVE=1, MODE_CONFIRM_DELETE=2, MODE_READ=3, MODE_PRIO_EDIT=4, MODE_NAV=5, MODE_SCREENSAVER=6 };
Mode mode = MODE_NORMAL;
enum TabPage : uint8_t { TAB_TASKS=0, TAB_COMPLETED=1 };
TabPage currentPage = TAB_TASKS; // default page
bool headerFocused = false; // when true, UP landed on header
int8_t   prioEditDir = 0;
uint32_t g_prioNextBumpMs = 0;

// ---------- Selection / Scrolling ----------
uint8_t topIndex = 0;
uint8_t selected = 0;

// ---------- Clock ----------
uint8_t  startHour = 8, startMin = 0;
uint32_t clockStartMs = 0;

// ---------- Canvases ----------
GFXcanvas16 headerCanvas(W, HEADER_HEIGHT);
GFXcanvas16 listCanvas(W, LIST_CANVAS_H);

// ---------- Modal / Input ----------
uint32_t confirmIgnoreUntil = 0;
uint32_t confirmDeleteUid   = 0;
uint32_t inputSquelchUntil  = 0;

// ---------- Filtered view helpers ----------
static inline bool isVisibleInCurrentPage(uint8_t idx) {
  if (idx >= taskCount) return false;
  return (currentPage == TAB_TASKS) ? (!tasks[idx].done) : tasks[idx].done;
}
static int findNextVisible(int startIdx, int dir) {
  int i = startIdx + dir;
  while (i >= 0 && i < (int)taskCount) {
    if (isVisibleInCurrentPage((uint8_t)i)) return i;
    i += dir;
  }
  return -1;
}
static int visibleOffsetFromTop(uint8_t top, uint8_t sel) {
  if (sel >= taskCount) return -1;
  int offset = 0;
  for (uint8_t i = top; i < taskCount && offset < VISIBLE_ROWS; ++i) {
    if (isVisibleInCurrentPage(i)) {
      if (i == sel) return offset;
      offset++;
    }
  }
  return -1;
}
static uint8_t computeTopIndexForSelection(uint8_t sel) {
  if (taskCount == 0) return 0;
  if (sel >= taskCount) sel = taskCount - 1;
  int needBefore = (int)VISIBLE_ROWS - 1;
  int before = 0;
  int t = (int)sel;
  for (int i = (int)sel - 1; i >= 0 && before < needBefore; --i) {
    if (isVisibleInCurrentPage((uint8_t)i)) before++;
    t = i;
  }
  if (t < 0) t = 0;
  return (uint8_t)t;
}

static void ensureSelectionValidForPage() {
  if (taskCount == 0) { selected = 0; topIndex = 0; return; }
  if (!isVisibleInCurrentPage(selected)) {
    int down = findNextVisible((int)selected, +1);
    int up   = findNextVisible((int)selected, -1);
    int pick = (down >= 0) ? down : up;
    if (pick < 0) {
      // Find first visible from start of list
      for (uint8_t i=0;i<taskCount;i++) if (isVisibleInCurrentPage(i)) { pick = i; break; }
    }
    if (pick < 0) { selected = 0; topIndex = 0; headerFocused = true; return; }
    selected = (uint8_t)pick;
  }
  topIndex = computeTopIndexForSelection(selected);
}

// ---------- Nav mode ----------
uint8_t navIndex = 0; // 0..3
uint8_t g_leftClicks = 0; uint32_t g_leftLastClickMs = 0;
// Nav scrolling (window of 4 visible icons over total)
static const uint8_t NAV_VISIBLE = 4;
static const uint8_t NAV_TOTAL   = 5; // 4 icons + battery
uint8_t navScrollTop = 0; // first visible index
uint8_t batteryPct = 100; // placeholder battery percent

// ---------- Pending animation ----------
const uint16_t PENDING_DELAY_MS = 2000;
const uint16_t FADE_ANIM_MS     = 300;

// ---------- OK multi-click ----------
uint8_t  g_okClicks = 0;
uint32_t g_okLastClickMs = 0;
uint32_t g_okAnchorUid   = 0;

// ---------- UI persistence debounce ----------
bool     uiDirty = false;
uint32_t lastUISaveMs = 0;
const uint16_t UI_SAVE_DEBOUNCE_MS = 1000;

// ---------- Read (full-text) ----------
const uint16_t READ_HOLD_MS = 1500;

// ---------- Marquee ----------
const uint16_t MARQUEE_IDLE_MS = 7000;
const uint16_t MARQUEE_SPEED_PX_PER_S= 28;
const uint16_t MARQUEE_GAP_PX = 24;
const uint16_t MARQUEE_FRAME_MS = 40;
uint32_t g_selStableSince = 0;
bool     g_marqueeActive = false;
uint32_t g_marqueeStartMs = 0;
uint32_t g_lastMarqueeFrame = 0;
int16_t  g_marqueeOffsetPx = 0;
// Screensaver
const uint32_t SCREENSAVER_IDLE_MS = 30000; // 30s
uint32_t g_lastUserInputMs = 0;
uint32_t g_saverAnimStartMs = 0;
uint8_t  g_mockBatteryPhase = 0; // 0..2 -> red, orange, green

// ---------- Serial ----------
const uint32_t SERIAL_BAUD = 115200;
static bool     g_serialImport = false;
static char     g_lineBuf[256];
static uint16_t g_lineLen = 0;

// ==== utils ====
static inline void markUIDirty() { uiDirty = true; }
static inline float   easeOutCubic(float t){ t = 1.0f - t; return 1.0f - t*t*t; }
static inline int16_t i16min(int16_t a, int16_t b){ return (a<b)?a:b; }
static inline int16_t i16max(int16_t a, int16_t b){ return (a>b)?a:b; }

// ---------- Display wrapper ----------
class ST7789_Ex : public Adafruit_ST7789 {
 public:
  ST7789_Ex(int8_t cs, int8_t dc, int8_t rst) : Adafruit_ST7789(cs, dc, rst) {}
  void setOffset(int8_t col, int8_t row) { setColRowStart(col, row); } // expose protected
} tft(TFT_CS, TFT_DC, TFT_RST);

// ---------- Persistence ----------
void saveUIState() {
  prefs.begin("myday", false);
  prefs.putUChar("sel",  selected);
  prefs.putUChar("top",  topIndex);
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
  // load brightness (default 60)
  g_blPct = prefs.getUChar("bl_pct", 60);
  prefs.end();

  if (g_blPct < BL_MIN_PCT) g_blPct = BL_MIN_PCT;
  if (g_blPct > BL_MAX_PCT) g_blPct = BL_MAX_PCT;

  if (taskCount == 0) {
    selected = 0; topIndex = 0; return;
  }
  if (sel >= taskCount) sel = taskCount - 1;
  selected = sel;
  if (top > selected) top = selected;
  if (selected >= top + VISIBLE_ROWS) top = selected - VISIBLE_ROWS + 1;
  topIndex = top;
}

// ====== seed tasks ======
struct SeedTask { const char* text; bool done; uint8_t prio; };
const SeedTask SEED[] = {
  {"USPS money order", false, PRI_HIGH},
  {"Get groceries", false, PRI_HIGH},
  {"After work walk", false, PRI_MED},
  {"Clean cat litter", true, PRI_LOW},
  {"Clean car", false, PRI_LOW},
  {"Code tasklist (long sample to test marquee & read modal)", false, PRI_LOW},
};
const uint8_t SEED_COUNT = sizeof(SEED)/sizeof(SEED[0]);

static inline uint32_t fnv1a_update(uint32_t h, const uint8_t* d, size_t n){
  while(n--) { h ^= *d++; h *= 16777619UL; }
  return h;
}
uint32_t computeSeedHash() {
  uint32_t h = 2166136261UL;
  for (uint8_t i=0;i<SEED_COUNT;i++) {
    const char* s = SEED[i].text;
    h = fnv1a_update(h, (const uint8_t*)s, strlen(s));
    uint8_t d = SEED[i].done ? 1 : 0; h = fnv1a_update(h, &d, 1);
    uint8_t p = SEED[i].prio;        h = fnv1a_update(h, &p, 1);
  }
  uint8_t c = SEED_COUNT; h = fnv1a_update(h, &c, 1);
  return h;
}

void applySeedIfChangedAndLoad() {
  uint32_t newHash = computeSeedHash();
  prefs.begin("myday", false);
  uint32_t oldHash    = prefs.getUInt("hash", 0xFFFFFFFFUL);
  uint8_t  storedCnt  = prefs.getUChar("count", 0);
  String   storedBuild= prefs.getString("build", "");
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

  // Load
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
    tasks[i].uid = nextUid++;
  }
  prefs.end();

  if (selected >= taskCount) selected = taskCount ? (taskCount - 1) : 0;
  if (topIndex > selected) topIndex = selected;
}

// ---------- Buttons FSM ----------
namespace Btn {
  enum EventBits : uint8_t { EV_NONE=0, EV_EDGE_PRESS=1<<0, EV_EDGE_REL=1<<1, EV_CLICK=1<<2, EV_HOLD_START=1<<3, EV_REPEAT=1<<4 };
  struct FSM {
    uint8_t pin; bool raw=false; bool debPressed=false; bool armedClick=false;
    uint32_t tLastEdge=0, tPressStart=0, tReleaseStart=0, tHold0=0, nextRpt=0;
    bool repeating=false; uint8_t ev=EV_NONE;
  };
  static inline void init(FSM &b) {
    pinMode(b.pin, INPUT_PULLUP);
    b.raw = (digitalRead(b.pin) == LOW);
    b.debPressed=false; b.armedClick=false; b.tLastEdge=b.tPressStart=b.tReleaseStart=millis();
    b.repeating=false; b.tHold0=0; b.nextRpt=0; b.ev=EV_NONE;
  }
  static inline void drain(FSM &b){ b.armedClick=false; b.debPressed=false; b.repeating=false; b.ev=EV_NONE; }
  static inline void update(FSM &b){
    b.ev = EV_NONE;
    bool r = (digitalRead(b.pin) == LOW);
    uint32_t now = millis();
    if (r != b.raw) {
      if (now - b.tLastEdge >= DEBOUNCE_MS) {
        b.raw = r; b.tLastEdge = now;
        if (b.raw) b.tPressStart = now; else b.tReleaseStart = now;
      }
    }
    if (!b.debPressed && b.raw && (now - b.tPressStart >= MIN_PRESS_MS)) {
      b.debPressed = true; b.armedClick = true; b.repeating = false; b.tHold0 = now; b.nextRpt = now + HOLD_START_MS;
      b.ev |= EV_EDGE_PRESS;
    }
    if (b.debPressed && !r && (now - b.tReleaseStart >= MIN_RELEASE_MS)) {
      b.debPressed = false; b.repeating = false; b.ev |= EV_EDGE_REL;
      if (b.armedClick) { b.armedClick = false; b.ev |= EV_CLICK; }
    }
    if (b.debPressed) {
      if (!b.repeating) {
        if ((int32_t)(now - b.nextRpt) >= 0) {
          b.repeating = true; b.nextRpt = now + REPEAT_PERIOD_S;
          b.ev |= EV_HOLD_START; b.ev |= EV_REPEAT;
        }
      } else {
        uint32_t held = now - b.tHold0;
        uint16_t period = (held >= REPEAT_ACCEL2) ? REPEAT_PERIOD_F
                          : (held >= REPEAT_ACCEL1) ? (REPEAT_PERIOD_S * 2) / 3
                          : REPEAT_PERIOD_S;
        if ((int32_t)(now - b.nextRpt) >= 0) {
          b.nextRpt = now + period; b.ev |= EV_REPEAT;
        }
      }
    }
  }
  static inline bool pressed(const FSM& b){ return b.ev & EV_EDGE_PRESS; }
  static inline bool released(const FSM& b){ return b.ev & EV_EDGE_REL; }
  static inline bool clicked(const FSM& b){ return b.ev & EV_CLICK; }
  static inline bool repeat(const FSM& b){ return b.ev & EV_REPEAT; }
  static inline bool holdStart(const FSM& b){ return b.ev & EV_HOLD_START; }
  static inline bool heldFor(const FSM& b, uint32_t ms){ return b.debPressed && (millis() - b.tPressStart >= ms); }
  static inline uint32_t lastPressDurationMs(const FSM& b){
    return (b.tReleaseStart >= b.tPressStart) ? (b.tReleaseStart - b.tPressStart) : 0;
  }
}
Btn::FSM bUp{BTN_UP}, bOk{BTN_OK}, bDown{BTN_DOWN}, bLeft{BTN_LEFT}, bRight{BTN_RIGHT};

// ---------- Color helpers ----------
static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((uint16_t)((r * 31 + 127) / 255) << 11) | ((uint16_t)((g * 63 + 127) / 255) << 5) | ((uint16_t)((b * 31 + 127) / 255));
}
static inline void toRGB8(uint16_t c, uint8_t &r, uint8_t &g, uint8_t &b) {
  r = ((c >> 11) & 0x1F) * 255 / 31;
  g = ((c >> 5) & 0x3F) * 255 / 63;
  b = ( c & 0x1F) * 255 / 31;
}
static inline uint16_t fade565(uint16_t fg, uint16_t bg, uint8_t alpha) {
  uint8_t rF,gF,bF, rB,gB,bB; toRGB8(fg, rF,gF,bF); toRGB8(bg, rB,gB,bB);
  uint8_t r = (uint16_t(rF) * alpha + uint16_t(rB) * (255 - alpha) + 127) / 255;
  uint8_t g = (uint16_t(gF) * alpha + uint16_t(gB) * (255 - alpha) + 127) / 255;
  uint8_t b = (uint16_t(bF) * alpha + uint16_t(bB) * (255 - alpha) + 127) / 255;
  return rgb565(r,g,b);
}

// ---------- Alpha blitter ----------
static inline uint16_t lerp565(uint16_t fg, uint16_t bg, uint8_t a) {
  uint8_t rF = ((fg >> 11) & 0x1F) * 255 / 31;
  uint8_t gF = ((fg >> 5) & 0x3F) * 255 / 63;
  uint8_t bF = ( fg & 0x1F) * 255 / 31;
  uint8_t rB = ((bg >> 11) & 0x1F) * 255 / 31;
  uint8_t gB = ((bg >> 5) & 0x3F) * 255 / 63;
  uint8_t bB = ( bg & 0x1F) * 255 / 31;
  uint8_t r = (uint16_t(rF) * a + uint16_t(rB) * (255 - a) + 127) / 255;
  uint8_t g = (uint16_t(gF) * a + uint16_t(gB) * (255 - a) + 127) / 255;
  uint8_t b = (uint16_t(bF) * a + uint16_t(bB) * (255 - a) + 127) / 255;
  return ((uint16_t)((r * 31 + 127) / 255) << 11) | ((uint16_t)((g * 63 + 127) / 255) << 5) | ((uint16_t)((b * 31 + 127) / 255));
}

// ---------- Pixel-art nav icons (15x15, '.' off, 'X' on) ----------
// 5x5 masks scaled to 15x15 (3px blocks) for an 8-bit look
// 8-bit heart (single frame)
static const char ICON_HEART_5[5][6] PROGMEM = {
  ".X.X.",
  "XXXXX",
  "XXXXX",
  ".XXX.",
  "..X.."
};

// Microchip (single frame)
static const char ICON_CHIP_5[5][6] PROGMEM = {
  "XXXXX",
  "X...X",
  "X.X.X",
  "X...X",
  "XXXXX"
};

// Target (single frame)
static const char ICON_TARGET_5[5][6] PROGMEM = {
  ".XXX.",
  "X...X",
  "X.X.X",
  "X...X",
  ".XXX."
};

// Skull (single frame)
static const char ICON_SKULL_5[5][6] PROGMEM = {
  ".XXX.",
  "X.X.X",
  "XXXXX",
  ".X.X.",
  ".X.X."
};
static void drawIcon15(Adafruit_GFX &canvas, int16_t left, int16_t top, uint8_t which, uint16_t color) {
  const char (*icon)[6];
  switch (which) {
    case 0: icon = ICON_HEART_5;  break;
    case 1: icon = ICON_CHIP_5;   break;
    case 2: icon = ICON_TARGET_5; break;
    default: icon = ICON_SKULL_5; break;
  }
  const uint8_t cell = 3; // 3x3 blocks -> 15x15
  for (uint8_t y=0; y<5; ++y) {
    for (uint8_t x=0; x<5; ++x) {
      char c = pgm_read_byte(&(icon[y][x]));
      if (c != '.') {
        canvas.fillRect(left + x*cell, top + y*cell, cell, cell, color);
      }
    }
  }
}

// Alpha-matte sub-rect blit into a GFX (with bg color)
static inline void blit565_matte_subrect(Adafruit_GFX &gfx, int16_t dx, int16_t dy,
  const uint16_t *rgb, const uint8_t *alpha, uint16_t srcW, uint16_t srcH,
  uint16_t sx, uint16_t sy, uint16_t blitW, uint16_t blitH, uint16_t bg)
{
  uint16_t gw = gfx.width(), gh = gfx.height();
  for (uint16_t yy = 0; yy < blitH; ++yy) {
    int16_t gy = dy + yy; if (gy < 0 || gy >= gh) continue;
    uint32_t srow = (uint32_t)(sy + yy) * srcW;
    for (uint16_t xx = 0; xx < blitW; ++xx) {
      int16_t gx = dx + xx; if (gx < 0 || gx >= gw) continue;
      uint32_t si = srow + (sx + xx);
      uint8_t a = pgm_read_byte(&alpha[si]); if (!a) continue;
      uint16_t c = pgm_read_word(&rgb[si]);
      gfx.drawPixel(gx, gy, (a == 255) ? c : lerp565(c, bg, a));
    }
  }
}

// ---------- Header ----------
static void drawNavOnCanvas(Adafruit_GFX &canvas, int16_t canvasTopGlobalY) {
  // Panel region
  int16_t panelX = 0; // extend to left edge
  int16_t panelY = NAV_PAD - canvasTopGlobalY;
  int16_t panelW = NAV_WIDTH;
  int16_t panelH = NAV_HEIGHT;
  // Clip Y
  int16_t visY0 = 0;
  int16_t visY1 = canvas.height();
  int16_t drawY0 = i16max(panelY, visY0);
  int16_t drawY1 = i16min(panelY + panelH, visY1);
  if (drawY1 > drawY0) {
    // Draw the 32x170 navigation panel with rounded corners without mid-slice arcs
    const int16_t r = 10;
    int16_t sliceH = drawY1 - drawY0;
    bool isTop    = (drawY0 == panelY);
    bool isBottom = (drawY1 == panelY + panelH);

    // Base fill for this slice
    canvas.fillRect(panelX, drawY0, panelW, sliceH, COL_NAV_BAR);

    // Add rounded corners only where needed
    if (isTop) {
      int16_t h = (2*r < sliceH) ? (2*r) : sliceH;
      if (h > 0) canvas.fillRoundRect(panelX, drawY0, panelW, h, r, COL_NAV_BAR);
    }
    if (isBottom) {
      int16_t h = (2*r < sliceH) ? (2*r) : sliceH;
      if (h > 0) canvas.fillRoundRect(panelX, drawY1 - h, panelW, h, r, COL_NAV_BAR);
    }
  }

  // Icon geometry (15x15 pixel-art icons)
  const int16_t iconSize = 15;
  // Center icons within full panel width (panel starts at x=0 now)
  int16_t centerX = NAV_WIDTH/2;
  uint32_t nowMs = millis();

  // Even spacing between icons and nav edges: NAV_HEIGHT = N*h + (N+1)*gap
  const int16_t numIcons = NAV_VISIBLE;
  int16_t totalIconsH = NAV_VISIBLE * iconSize;
  int16_t available = NAV_HEIGHT - totalIconsH;
  if (available < 0) available = 0;
  const int16_t gapCount = numIcons + 1; // top + between + bottom
  int16_t gap = available / gapCount;
  int16_t remainder = available - gap * gapCount;
  int16_t startOffset = remainder / 2; // center leftover
  int16_t top0Global = NAV_PAD + gap + startOffset;

  for (uint8_t vis=0; vis<NAV_VISIBLE; ++vis) {
    uint8_t i = navScrollTop + vis;
    int16_t topGlobal = top0Global + vis * (iconSize + gap);
    int16_t left = centerX - iconSize/2;
    int16_t drawY = topGlobal - canvasTopGlobalY;
    if (drawY + iconSize < 0 || drawY >= canvas.height()) continue;

    // Selection indicator bar on the left of the icon (same height as icon)
    if (i == navIndex) {
      int16_t barW = 3; // thin rectangle
      int16_t barX = 0; // flush with left edge
      canvas.fillRect(barX, drawY, barW, iconSize, COL_NAV_SEL);
    }

    uint16_t iconColor = (i == navIndex)
      ? (i==0?COL_NAV_ICON0 : i==1?COL_NAV_ICON1 : i==2?COL_NAV_ICON2 : i==3?COL_NAV_ICON3 : COL_NAV_ICON0)
      : COL_ICON_GREY;
    if (i < 4) {
      drawIcon15(canvas, left, drawY, i, iconColor);
    } else {
      // Placeholder icon (8-bit diamond) using color #eebebe
      uint16_t colPH = tft.color565(0xEE, 0xBE, 0xBE);
      // 5x5 diamond mask centered within 15x15, using 3x3 blocks to match other icons
      static const uint8_t DIAMOND[5][5] = {
        {0,0,1,0,0},
        {0,1,1,1,0},
        {1,1,1,1,1},
        {0,1,1,1,0},
        {0,0,1,0,0}
      };
      for (uint8_t ry=0; ry<5; ++ry) {
        for (uint8_t rx=0; rx<5; ++rx) {
          if (DIAMOND[ry][rx]) canvas.fillRect(left + rx*3, drawY + ry*3, 3, 3, colPH);
        }
      }
    }
  }

  // 8-bit triangle scroll indicators (blocky, symmetric)
  bool canScrollUp = (navScrollTop > 0);
  bool canScrollDown = (navScrollTop + NAV_VISIBLE < NAV_TOTAL);
  int16_t cx = centerX;
  const int16_t arrowH = 7;  // rows
  const int16_t arrowW = 11; // base width
  // Compute slice and icon bounds in this canvas
  int16_t sliceTop = drawY0;
  int16_t sliceBot = drawY1;
  int16_t topOfIconLocal = top0Global - canvasTopGlobalY;
  int16_t bottomOfIconLocal = top0Global + (NAV_VISIBLE - 1) * (iconSize + gap) + iconSize - canvasTopGlobalY;

  // Arrow color (#babbf1)
  uint16_t ARROW_COL = tft.color565(0xBA, 0xBB, 0xF1);

  // Only draw top arrow in header slice; bottom arrow in list slice to avoid duplicates
  bool isHeaderSlice = (canvasTopGlobalY == 0);
  bool isListSlice   = (canvasTopGlobalY == HEADER_HEIGHT);

  if (canScrollUp && isHeaderSlice) {
    int16_t topSpan = topOfIconLocal - sliceTop;
    int16_t pad = (topSpan > arrowH) ? (topSpan - arrowH) / 2 : 1;
    pad += 5; // extra spacing from icon
    if (pad > 10) pad = 10; // up to +10px extra spacing
    int16_t baseY = topOfIconLocal - pad; // widest row closest to icon
    // Up arrow: draw from base upward decreasing width, 2px-thick rows
    for (int8_t r = 0; r < arrowH; ++r) {
      int16_t w = arrowW - 2 * r; // 11,9,7,5,3,1
      if (w < 1) break;
      int16_t y = baseY - r;
      int16_t half = (w - 1) / 2;
      int16_t x0 = cx - half;
      if (y >= sliceTop && y < sliceBot) {
        canvas.drawFastHLine(x0, y, w, ARROW_COL);
        if (y - 1 >= sliceTop) canvas.drawFastHLine(x0, y - 1, w, ARROW_COL); // blockier row without bleeding above slice
      }
    }
  }
  if (canScrollDown && isListSlice) {
    int16_t bottomSpan = sliceBot - bottomOfIconLocal;
    int16_t pad = (bottomSpan > arrowH) ? (bottomSpan - arrowH) / 2 : 1;
    pad += 5; // extra spacing from icon
    if (pad > 10) pad = 10; // up to +10px extra spacing
    int16_t tipY = bottomOfIconLocal + pad; // tip (narrowest row)
    // Down arrow: symmetric, 2px-thick rows
    for (int8_t r = 0; r < arrowH; ++r) {
      int16_t w = arrowW - 2 * r; // 11,9,7,5,3,1...
      if (w < 1) break;
      int16_t y = tipY + r;
      int16_t half = (w - 1) / 2;
      int16_t x0 = cx - half;
      if (y >= 0 && y < canvas.height()) {
        canvas.drawFastHLine(x0, y, w, ARROW_COL);
        if (y + 1 < canvas.height()) canvas.drawFastHLine(x0, y + 1, w, ARROW_COL);
      }
    }
  }
  
  // Only icons and the 3px divider
}
void rebuildHeader(bool force=false) {
  static char lastTime[12] = "";
  static Mode lastMode = MODE_NORMAL;

  auto formatTime = [](char* out, size_t n, uint8_t h24, uint8_t m){
    bool pm = (h24 >= 12); uint8_t h = h24 % 12; if (h == 0) h = 12;
    snprintf(out, n, "%u:%02u %s", h, m, pm ? "PM" : "AM");
  };

  uint32_t elapsedMs = millis() - clockStartMs;
  uint32_t totalMin = startHour * 60UL + startMin + (elapsedMs / 60000UL);
  uint8_t hh = (totalMin / 60U) % 24U;
  uint8_t mm = (uint8_t)(totalMin % 60U);
  char nowStr[12]; formatTime(nowStr, sizeof(nowStr), hh, mm);

  if (!force && (strcmp(nowStr, lastTime) == 0) && (lastMode == mode) && (millis() > g_blOsdUntil)) return;

  headerCanvas.fillRect(0,0,W,HEADER_HEIGHT, COL_BG);

  // Draw icons + divider (no nav panel)
  drawNavOnCanvas(headerCanvas, /*canvasTopGlobalY=*/0);

  // MAIN_LEFT reference
  int16_t MAINL = MAIN_LEFT();

  // Header label
  headerCanvas.setTextWrap(false);
  headerCanvas.setTextSize(2);
  // Align left bracket with list brackets
  int16_t curX = MAINL + BRACKET_X_TWEAK;

  // Build label string (override in MOVE mode)
  const char* label = (mode == MODE_MOVE) ? "Move" : ((currentPage == TAB_TASKS) ? "Task" : "Completed");
  char fullLbl[32]; snprintf(fullLbl, sizeof(fullLbl), "[ %s ]", label);

  // Measure text for tight highlight
  int16_t tbx, tby; uint16_t tw, th;
  headerCanvas.getTextBounds(fullLbl, 0, 0, &tbx, &tby, &tw, &th);
  int16_t padX = 6, padY = 2;
  // No background fill when focused; only brackets change color

  // Draw "[ label ]" with original colors; special styling in MOVE mode
  headerCanvas.setCursor(curX, HEADER_Y_PAD);
  if (mode == MODE_MOVE) {
    // Brackets and label use pill color
    headerCanvas.setTextColor(COL_MOVE, COL_BG); headerCanvas.print("[ ");
    headerCanvas.setTextColor(COL_MOVE, COL_BG); headerCanvas.print(label);
    headerCanvas.setTextColor(COL_MOVE, COL_BG); headerCanvas.print(" ]");
  } else if (headerFocused) {
    // Brackets green, label as COL_HIL on BG
    headerCanvas.setTextColor(COL_TAB_HIL, COL_BG); headerCanvas.print("[ ");
    headerCanvas.setTextColor(COL_HIL,      COL_BG); headerCanvas.print(label);
    headerCanvas.setTextColor(COL_TAB_HIL, COL_BG); headerCanvas.print(" ]");
  } else {
    headerCanvas.setTextColor(COL_WHITE, COL_BG); headerCanvas.print("[ ");
    headerCanvas.setTextColor(COL_HIL,   COL_BG); headerCanvas.print(label);
    headerCanvas.setTextColor(COL_WHITE, COL_BG); headerCanvas.print(" ]");
  }

  // When focused, draw a pixel chevron arrow indicating direction
  // (removed per request)

  // Time, aligned to right
  int16_t x1,y1; uint16_t w,hb;
  headerCanvas.getTextBounds(nowStr, 0, 0, &x1, &y1, &w, &hb);
  int16_t tx = W - 12 - (int16_t)w + RIGHT_TWEAK_PX;
  headerCanvas.setCursor(tx, HEADER_Y_PAD);
  headerCanvas.setTextColor(COL_CLOCK, COL_BG);
  headerCanvas.print(nowStr);

  // Brightness OSD moved to centered modal
 tft.drawRGBBitmap(0, 0, headerCanvas.getBuffer(), W, HEADER_HEIGHT);

  strncpy(lastTime, nowStr, sizeof(lastTime)); lastTime[sizeof(lastTime)-1] = '\0';
  lastMode = mode;
}

// ---------- Footer counts ----------
uint16_t countDone() { uint16_t k=0; for (uint8_t i=0;i<taskCount;i++) if (tasks[i].done) k++; return k; }

// ---------- Priority colors ----------
inline uint16_t colorForPriority(uint8_t pr) {
  if (pr == PRI_HIGH) return COL_PRI_HIGH;
  if (pr == PRI_MED)  return COL_PRI_MED;
  return COL_WHITE;
}

// ---- Text area ----
void computeTextArea(int16_t &textX, int16_t &availW) {
  int16_t MAINL = MAIN_LEFT();
  listCanvas.setTextSize(2);
  int16_t lbx,lby, xbx,xby, rbx,rby; uint16_t lW,lH, xW,xH, rW,rH;
  listCanvas.getTextBounds("[", 0, 0, &lbx, &lby, &lW, &lH);
  listCanvas.getTextBounds("X", 0, 0, &xbx, &xby, &xW, &xH);
  listCanvas.getTextBounds("]", 0, 0, &rbx, &rby, &rW, &rH);
  listCanvas.getTextBounds(" ", 0, 0, &lbx, &lby, &lW, &lH); // reuse
  int16_t insideW = (xW > lW) ? (int16_t)xW : (int16_t)lW;
  insideW += (int16_t)BAR_EXPAND_X; if (insideW < 1) insideW = 1;
  int16_t textX0 = MAINL + (int16_t)lW + insideW + (int16_t)rW + 8;
  textX = textX0 + TASK_TEXT_X_TWEAK;
  availW = W - 14 - textX; if (availW < 1) availW = 1;
}

// ---------- Row draw ----------
void drawRowContentToCanvas(uint8_t taskIndex, int16_t y, int fadeAlpha, uint16_t fadeBg, int16_t marqueeOffsetPx, int16_t MAINL) {
  if (taskIndex >= taskCount) return;

  listCanvas.setTextWrap(false);
  listCanvas.setTextSize(2);

  int16_t lbx,lby, xbx,xby, rbx,rby; uint16_t lW,lH, xW,xH, rW,rH;
  listCanvas.getTextBounds("[", 0, 0, &lbx, &lby, &lW, &lH);
  listCanvas.getTextBounds("X", 0, 0, &xbx, &xby, &xW, &xH);
  listCanvas.getTextBounds("]", 0, 0, &rbx, &rby, &rW, &rH);

  int16_t cbh = lH; if (xH > cbh) cbh = xH; if (rH > cbh) cbh = rH;

  int16_t checkTop = y + ((ROW_HEIGHT - cbh) / 2) + TEXT_Y_TWEAK + BRACKET_Y_TWEAK;
  int16_t baseL = checkTop - lby;
  int16_t baseR = checkTop - rby;
  int16_t checkX = MAINL + BRACKET_X_TWEAK;
  int16_t xInsideStart = checkX + (int16_t)lW;
  int16_t xRightPos    = xInsideStart + ( (xW > (uint16_t)8 ? (int16_t)xW : 8) + (int16_t)BAR_EXPAND_X );

  bool isSelected = (taskIndex == selected);

  uint16_t colBracket = colorForPriority(tasks[taskIndex].prio);
  uint16_t colMark    = isSelected ? COL_X_HARD : COL_FOOT;
  uint16_t colText    = tasks[taskIndex].done ? COL_DIM : COL_WHITE;
  uint16_t colStrike  = COL_DIM;

  if (fadeAlpha >= 0) {
    uint8_t a = (uint8_t)fadeAlpha;
    colBracket = fade565(colBracket, fadeBg, a);
    colMark    = fade565(colMark,    fadeBg, a);
    colText    = fade565(colText,    fadeBg, a);
    colStrike  = fade565(colStrike,  fadeBg, a);
  }

  // done mark
  if (tasks[taskIndex].done) {
    int16_t left = xInsideStart;
    int16_t w = xRightPos - xInsideStart;
    int16_t cx = left + w/2;
    int16_t cy = checkTop + cbh/2;

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
        int16_t barX = left - (int16_t)BAR_UNDERLAP_L + (int16_t)BAR_MARGIN_X + BRACKET_X_BIAS;
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
        drawThickLine(cx - span/2, cy, cx + span/2, cy, colMark, LINE_THICK);
        drawThickLine(cx, cy - span/2, cx, cy + span/2, colMark, LINE_THICK);
        drawThickLine(cx - span/2, cy - span/2, cx + span/2, cy + span/2, colMark, LINE_THICK);
        drawThickLine(cx - span/2, cy + span/2, cx + span/2, cy - span/2, colMark, LINE_THICK);
      } break;
      case DONE_DOT: {
        listCanvas.fillCircle(cx, cy, DOT_RADIUS, colMark);
      } break;
    }
  }

  // Brackets
  listCanvas.setTextColor(colBracket);
  listCanvas.setCursor(checkX, baseL); listCanvas.print("[");
  listCanvas.setCursor(xRightPos, baseR); listCanvas.print("]");

  // Text area
  const int16_t textX0_calc = MAINL + (int16_t)lW + (xRightPos - xInsideStart) + (int16_t)rW + 8;
  const int16_t textX0 = textX0_calc + TASK_TEXT_X_TWEAK;
  int16_t availW = W - 14 - textX0; if (availW < 1) availW = 1;

  if (marqueeOffsetPx >= 0 && isSelected) {
    int16_t pillH = cbh + 2 * (int16_t)HIL_GAP;
    int16_t rowCenterY = y + (int16_t)ROW_HEIGHT/2;
    int16_t pillY = rowCenterY - cbh/2 - (int16_t)HIL_GAP + TEXT_Y_TWEAK + BRACKET_Y_TWEAK;

    int16_t bx, by; uint16_t fullW, fullH;
    listCanvas.getTextBounds(tasks[taskIndex].text, 0, 0, &bx, &by, &fullW, &fullH);
    int16_t textYlocal = ((pillH - (int16_t)fullH) / 2) - by + TEXT_Y_TWEAK;

    GFXcanvas1 mc(availW, pillH); mc.fillScreen(0);
    mc.setTextWrap(false); mc.setTextSize(2); mc.setTextColor(1);
    int16_t x0 = -marqueeOffsetPx; mc.setCursor(x0, textYlocal); mc.print(tasks[taskIndex].text);
    mc.setCursor(x0 + (int16_t)fullW + (int16_t)MARQUEE_GAP_PX, textYlocal); mc.print(tasks[taskIndex].text);
    listCanvas.drawBitmap(textX0, pillY, mc.getBuffer(), mc.width(), mc.height(), colText);

    if (tasks[taskIndex].done) {
      int16_t mid = pillY + pillH/2; listCanvas.drawFastHLine(textX0, mid, availW, colStrike);
    }
  } else {
    char buf[24]; strncpy(buf, tasks[taskIndex].text, sizeof(buf)-1); buf[sizeof(buf)-1] = '\0';
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
    listCanvas.setCursor(textX0, textY); listCanvas.setTextColor(colText); listCanvas.print(buf);
    if (tasks[taskIndex].done) {
      int16_t x1,y1; uint16_t w2,h2; listCanvas.getTextBounds(buf, textX0, textY, &x1, &y1, &w2, &h2);
      int16_t mid = y1 + (int16_t)h2/2; listCanvas.drawFastHLine(textX0, mid, w2, colStrike);
    }
  }
}

// ---------- List compose/flush ----------

// ---------- Brightness modal (drawn on listCanvas to avoid flicker) ----------
static void drawBrightnessModalOnListCanvas() {
  // Compute segments based on clamped UI percent
  uint8_t pct = g_blPct;
  if (pct < BL_MIN_PCT) pct = BL_MIN_PCT;
  if (pct > BL_MAX_PCT) pct = BL_MAX_PCT;
  uint8_t segs = (uint8_t)((uint32_t)(pct - BL_MIN_PCT) * 10u / (uint32_t)(BL_MAX_PCT - BL_MIN_PCT)); // 0..10
  if (segs > 10) segs = 10;
  // Ensure at least one '=' so the bar never looks empty at minimum
  if (segs == 0) segs = 1;

  // Build the bar "[==========]" (no spaces inside)
  char bar[20];
  uint8_t i=0;
  bar[i++]='[';
  for (uint8_t k=0;k<segs;k++) bar[i++]='=';
  for (uint8_t k=segs;k<10;k++) bar[i++]=' ';
  bar[i++]=']';
  bar[i]=' ';

  const char* title = "Brightness";

  // Use listCanvas metrics (same font size as UI: 2x)
  listCanvas.setTextWrap(false);
  listCanvas.setTextSize(2);

  // Measure strings on canvas
  int16_t x1,y1, xa,ya, xb,yb;
  uint16_t wTitle,hTitle, wMinus,hMinus, wPlus,hPlus, wBar,hBar;
  listCanvas.getTextBounds(title, 0, 0, &x1, &y1, &wTitle, &hTitle);
  listCanvas.getTextBounds("-", 0, 0, &xa, &ya, &wMinus, &hMinus);
  listCanvas.getTextBounds("+", 0, 0, &xb, &yb, &wPlus, &hPlus);
  listCanvas.getTextBounds(bar, 0, 0, &xb, &yb, &wBar, &hBar);

  const int16_t padX = 14, padY = 14, gapY = 12, gapX = 10;
  int16_t rowW = (int16_t)wMinus + gapX + (int16_t)wBar + gapX + (int16_t)wPlus;
  uint16_t contentW = (wTitle > rowW) ? wTitle : (uint16_t)rowW;

  int16_t bw = (int16_t)contentW + padX*2;
  if (bw > (W - 2*8)) bw = W - 2*8; // clamp
  int16_t innerW = bw - padX*2;

  int16_t usedGapX = gapX;
  if (rowW > innerW) {
    int16_t extra = rowW - innerW;
    int16_t tryGap = gapX - extra/2;
    if (tryGap < 6) tryGap = 6;
    usedGapX = tryGap;
    rowW = (int16_t)wMinus + usedGapX + (int16_t)wBar + usedGapX + (int16_t)wPlus;
  }

  uint16_t maxH = hBar;
  if (hMinus > maxH) maxH = hMinus;
  if (hPlus  > maxH) maxH = hPlus;

  int16_t bh = (int16_t)hTitle + gapY + (int16_t)maxH + padY*2;
  int16_t bx = (W - bw) / 2;                          // same W as screen
  int16_t by = (LIST_CANVAS_H - bh) / 2;              // center within listCanvas

  // Panel & shadow on the canvas
  listCanvas.fillRoundRect(bx + 2, by + 3, bw, bh, 12, COL_SHADOW);
  uint16_t COL_PANEL = COL_MOVE;
  uint16_t COL_TXT   = COL_BLACK;
  listCanvas.fillRoundRect(bx, by, bw, bh, 10, COL_PANEL);
  listCanvas.drawRoundRect(bx, by, bw, bh, 10, COL_PANEL);

  // Title
  int16_t tx = bx + (bw - (int16_t)wTitle)/2;
  int16_t ty = by + padY - y1;
  listCanvas.setTextColor(COL_TXT, COL_PANEL);
  listCanvas.setCursor(tx, ty);
  listCanvas.print(title);

  // Row "- [bar] +"
  int16_t rowX = bx + (bw - rowW)/2;
  int16_t rowY = ty + (int16_t)hTitle + gapY - ya;
  listCanvas.setCursor(rowX, rowY);
  listCanvas.print("-");

  int16_t barX = rowX + (int16_t)wMinus + usedGapX;
  listCanvas.setCursor(barX, rowY - (yb - ya));
  listCanvas.print(bar);

  int16_t plusX = barX + (int16_t)wBar + usedGapX;
  listCanvas.setCursor(plusX, rowY - (yb - ya));
  listCanvas.print("+");
}

void composeListFrame(float hlRowY) {
  listCanvas.fillRect(0, 0, W, LIST_CANVAS_H, COL_BG);

  // Draw icons + divider on list canvas
  drawNavOnCanvas(listCanvas, /*canvasTopGlobalY=*/HEADER_HEIGHT);

  // MAIN_LEFT
  int16_t MAINL = MAIN_LEFT();

  // Highlight pill sizing
  listCanvas.setTextSize(2);
  int16_t lbx,lby, xbx,xby, rbx,rby; uint16_t lW,lH, xW,xH, rW,rH;
  listCanvas.getTextBounds("[", 0, 0, &lbx, &lby, &lW, &lH);
  listCanvas.getTextBounds("X", 0, 0, &xbx, &xby, &xW, &xH);
  listCanvas.getTextBounds("]", 0, 0, &rbx, &rby, &rW, &rH);
  int16_t cbh = lH; if (xH>cbh) cbh = xH; if (rH>cbh) cbh = rH;

  int16_t rowCenterY = (int16_t)(TOP_PAD + hlRowY + 0.5f);
  int16_t brTop = rowCenterY - cbh/2 + TEXT_Y_TWEAK + BRACKET_Y_TWEAK;
  int16_t brBottom = brTop + cbh;

  int16_t contentLeft  = MAINL - 3; // shift pill right by 3px to clear nav
  int16_t contentRight = W - 14 + 2;
  int16_t pillX = contentLeft, pillW = contentRight - contentLeft;
  int16_t pillY = brTop - (int16_t)HIL_GAP;
  int16_t pillH = (brBottom - brTop) + 2*(int16_t)HIL_GAP;
  const int8_t HIL_Y_NUDGE = -1;
  pillY += HIL_Y_NUDGE;
  if (pillY < 1) pillY = 1;
  if (pillY + pillH > LIST_CANVAS_H - 1) pillY = LIST_CANVAS_H - 1 - pillH;

  uint16_t pillCol = (mode == MODE_MOVE) ? COL_MOVE : COL_HIL;

  if (mode != MODE_NAV) {
    const int16_t shX = pillX + 1;
    const int16_t shY = pillY + 2;
    listCanvas.fillRoundRect(shX, shY, pillW, pillH, HIL_RADIUS + 1, COL_SHADOW);
    listCanvas.fillRoundRect(pillX, pillY, pillW, pillH, HIL_RADIUS, pillCol);
  }

#if ENABLE_PILL_PULSE
  if (mode != MODE_MOVE && mode != MODE_NAV) {
    uint32_t nowMs = millis();
    // Stronger, sharper pixel pulse
    uint8_t a = (uint8_t)((sinf((nowMs % 800) * 0.007853981f) * 0.5f + 0.5f) * 160.0f) + 80; // higher alpha range
    uint16_t pulseCol = fade565(COL_ACCENT, pillCol, a);
    // Pixelated stepped border
    listCanvas.drawRoundRect(pillX - 1, pillY - 1, pillW + 2, pillH + 2, HIL_RADIUS + 1, pulseCol);
    listCanvas.drawRoundRect(pillX - 3, pillY - 3, pillW + 6, pillH + 6, HIL_RADIUS + 2, fade565(pulseCol, COL_BG, 180));
  }
#endif

  if (HIL_BORDER_ENABLED && mode != MODE_MOVE && mode != MODE_NAV) {
    uint16_t borderCol = HIL_BORDER_ACCENT ? COL_ACCENT : COL_BORDER;
    listCanvas.drawRoundRect(pillX, pillY, pillW, pillH, HIL_RADIUS, borderCol);
  }

  uint32_t now = millis();
  for (uint8_t i=0, drawn=0; drawn<VISIBLE_ROWS; ) {
    uint8_t idx = topIndex + i;
    if (idx >= taskCount) break;
    bool show = (currentPage == TAB_TASKS) ? (!tasks[idx].done) : (tasks[idx].done);
    if (!show) { i++; continue; }
    int16_t rowY = TOP_PAD + drawn * ROW_HEIGHT;

    int fadeAlpha = -1;
    uint16_t fadeBg = (idx == selected) ? pillCol : COL_BG;

    if (idx < taskCount && tasks[idx].pend == PEND_FADE) {
      uint32_t elapsed = now - tasks[idx].animStartMs;
      if (elapsed >= FADE_ANIM_MS) fadeAlpha = 0;
      else {
        float t = (float)elapsed / (float)FADE_ANIM_MS; if (t < 0) t = 0; if (t > 1) t = 1;
        fadeAlpha = (uint8_t)(255.0f * (1.0f - t));
      }
    }

    int16_t mOffset = (g_marqueeActive && idx == selected) ? g_marqueeOffsetPx : -1;
    drawRowContentToCanvas(idx, rowY, fadeAlpha, fadeBg, mOffset, MAINL);
    drawn++; i++;
  }

  // Footer counter
  uint16_t remaining = 0, completed = 0;
  for (uint8_t i=0;i<taskCount;i++) { if (tasks[i].done) completed++; else remaining++; }
  uint16_t displayCount = (currentPage == TAB_TASKS) ? remaining : completed;
  char nums[20]; snprintf(nums, sizeof(nums), "%u", (unsigned)displayCount);
  char full[28]; snprintf(full, sizeof(full), "[ %s ]", nums);

  listCanvas.setTextSize(2);
  int16_t fx,fy; uint16_t fw,fh;
  listCanvas.getTextBounds(full, 0, 0, &fx, &fy, &fw, &fh);

  int16_t lx,ly,nx,ny,rx,ry; uint16_t lw,lh,nw,nh,rw,rh;
  listCanvas.getTextBounds("[ ", 0, 0, &lx, &ly, &lw, &lh);
  listCanvas.getTextBounds(nums, 0, 0, &nx, &ny, &nw, &nh);
  listCanvas.getTextBounds(" ]", 0, 0, &rx, &ry, &rw, &rh);

  int16_t tx = W - 14 - (int16_t)fw + RIGHT_TWEAK_PX;
  int16_t ty = LIST_CANVAS_H - (int16_t)FOOTER_BOTTOM_PAD - (int16_t)fh - fy;

  listCanvas.setTextColor(COL_WHITE);
  listCanvas.setCursor(tx, ty); listCanvas.print("[ ");
  listCanvas.setTextColor(COL_FOOT);
  listCanvas.setCursor(tx + (int16_t)lw, ty); listCanvas.print(nums);
  listCanvas.setTextColor(COL_WHITE);
  listCanvas.setCursor(tx + (int16_t)lw + (int16_t)nw, ty); listCanvas.print(" ]");

  // Overlay brightness modal onto listCanvas (flicker-free)
  if ((int32_t)(g_blOsdUntil - millis()) > 0 && mode != MODE_CONFIRM_DELETE && mode != MODE_READ) {
    drawBrightnessModalOnListCanvas();
  }

  // "[ + ]" action on bottom-left, aligned under left brackets
  {
    int16_t addX = MAINL + BRACKET_X_TWEAK; // align with row left bracket
    int16_t addY = ty; // align vertically with footer baseline
    uint16_t ADD_COL = tft.color565(0xBA, 0xBB, 0xF1); // #babbf1
    // Draw with white brackets and colored plus
    listCanvas.setCursor(addX, addY);
    listCanvas.setTextColor(COL_WHITE, COL_BG); listCanvas.print("[ ");
    listCanvas.setTextColor(ADD_COL,  COL_BG); listCanvas.print("+");
    listCanvas.setTextColor(COL_WHITE, COL_BG); listCanvas.print(" ]");
  }
}
inline void flushList() {
  tft.drawRGBBitmap(0, LIST_CANVAS_Y, listCanvas.getBuffer(), W, LIST_CANVAS_H);
}
void rebuildListStatic() {
  ensureSelectionValidForPage();
  int visOff = visibleOffsetFromTop(topIndex, selected);
  if (visOff < 0) visOff = 0;
  float hlRowY = (visOff + 0.5f) * ROW_HEIGHT;
  composeListFrame(hlRowY);
  flushList();
}

// ---------- Anim ----------
static void animateHighlightVis(int8_t startOff, int8_t endOff) {
  if (startOff < 0) startOff = endOff;
  float startY = (startOff + 0.5f) * ROW_HEIGHT;
  float endY   = (endOff   + 0.5f) * ROW_HEIGHT;
  for (uint8_t f=1; f<=ANIM_FRAMES; ++f) {
    float t = (float)f / ANIM_FRAMES;
    float e = easeOutCubic(t);
    float y = startY + (endY - startY) * e;
    composeListFrame(y); flushList(); delay(ANIM_DELAY_MS);
  }
}

// ---------- Movement & Helpers ----------
void ensureSelectionVisibleAndDraw(int8_t moveDir) {
  uint8_t oldTop = topIndex, oldSel = selected;
  // Move selection to next/prev visible item in the current page
  if (moveDir > 0) {
    int ns = findNextVisible((int)selected, +1);
    if (ns >= 0) selected = (uint8_t)ns;
  } else if (moveDir < 0) {
    int ps = findNextVisible((int)selected, -1);
    if (ps >= 0) selected = (uint8_t)ps;
  }
  // Adjust topIndex so that selected is within the visible window of filtered rows
  {
    int visOff = visibleOffsetFromTop(topIndex, selected);
    if (visOff < 0) {
      // selected not within current window; compute a new top
      topIndex = computeTopIndexForSelection(selected);
    } else {
      // Keep selected centered in window as much as possible by recomputing top
      topIndex = computeTopIndexForSelection(selected);
    }
  }
  if (selected != oldSel) { g_selStableSince = millis(); g_marqueeActive = false; }
  if (topIndex != oldTop || selected != oldSel) markUIDirty();
  int oldVis = visibleOffsetFromTop(oldTop, oldSel);
  int newVis = visibleOffsetFromTop(topIndex, selected);
  if (oldVis < 0) oldVis = newVis;
  if (topIndex != oldTop) rebuildListStatic();
  else if (selected != oldSel) animateHighlightVis((int8_t)oldVis, (int8_t)newVis);
}

inline void swapTasks(uint8_t i, uint8_t j) { Task tmp = tasks[i]; tasks[i] = tasks[j]; tasks[j] = tmp; }

void moveSelectedRow(int8_t dir) {
  if (dir < 0) {
    if (selected == 0) return;
    swapTasks(selected, selected - 1); selected--;
  } else {
    if (selected + 1 >= taskCount) return;
    swapTasks(selected, selected + 1); selected++;
  }
  if (selected >= topIndex + VISIBLE_ROWS) topIndex = selected - VISIBLE_ROWS + 1;
  if (selected < topIndex) topIndex = selected;
  g_selStableSince = millis(); g_marqueeActive = false; markUIDirty(); rebuildListStatic();
}

// ---- Delete helpers ----
int findIndexByUid(uint32_t uid) { for (uint8_t i=0;i<taskCount;i++) if (tasks[i].uid == uid) return i; return -1; }
void saveTasksRuntime();
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
  Task moved = tasks[idx]; moved.pend = PEND_NONE; moved.pendStartMs=0; moved.animStartMs=0;
  for (uint8_t i = idx; i < taskCount - 1; ++i) tasks[i] = tasks[i + 1];
  tasks[taskCount - 1] = moved;
  if (selected > idx) selected--;
  if (selected < topIndex) topIndex = selected;
  if (selected >= topIndex + VISIBLE_ROWS) topIndex = selected - VISIBLE_ROWS + 1;
  saveTasksRuntime(); markUIDirty();
}

// Toggle
void toggleByIndex(uint8_t idx) {
  if (idx >= taskCount) return;
  bool wasDone = tasks[idx].done;
  tasks[idx].done = !wasDone;
  // Delete/complete animation disabled: no pending states
  tasks[idx].pend = PEND_NONE;
  tasks[idx].pendStartMs = 0;
  tasks[idx].animStartMs = 0;
  saveTasksRuntime();
  // Maintain selection within the current filtered view
  {
    int nextSel = -1;
    if (currentPage == TAB_TASKS) {
      // If we just completed an item while in Tasks, select the next remaining task
      if (tasks[idx].done) {
        nextSel = findNextVisible((int)idx, +1);
        if (nextSel < 0) nextSel = findNextVisible((int)idx, -1);
      } else {
        // We un-completed an item while in Tasks (it remains visible): keep selection on it
        nextSel = (int)idx;
      }
    } else { // TAB_COMPLETED
      // If we just un-completed an item while in Completed, select the next completed
      if (!tasks[idx].done) {
        nextSel = findNextVisible((int)idx, +1);
        if (nextSel < 0) nextSel = findNextVisible((int)idx, -1);
      } else {
        // We completed an item while in Completed (it remains visible): keep selection on it
        nextSel = (int)idx;
      }
    }
    if (nextSel >= 0) {
      selected = (uint8_t)nextSel;
      topIndex = computeTopIndexForSelection(selected);
    } else {
      // No items left in this view; clamp indices safely
      selected = 0; topIndex = 0;
    }
  }
  g_selStableSince = millis(); g_marqueeActive = false; rebuildListStatic();
}
void toggleByUid(uint32_t uid) {
  if (uid == 0) return;
  for (uint8_t i=0; i<taskCount; ++i) if (tasks[i].uid == uid) { toggleByIndex(i); return; }
}

// ---------- Priority + sort ----------
void sortTasksByPriorityKeepSelection() {
  if (taskCount <= 1) return;
  uint32_t selUid = (selected < taskCount) ? tasks[selected].uid : 0;
  Task tmp[MAX_TASKS]; uint8_t n = 0;
  for (int pr = PRI_HIGH; pr >= PRI_LOW; --pr)
    for (uint8_t i=0; i<taskCount; ++i)
      if (!tasks[i].done && tasks[i].prio == (uint8_t)pr) tmp[n++] = tasks[i];
  for (uint8_t i=0; i<taskCount; ++i) if (tasks[i].done) tmp[n++] = tasks[i];
  for (uint8_t i=0; i<n; ++i) tasks[i] = tmp[i];
  if (selUid != 0) { int idx = findIndexByUid(selUid); if (idx >= 0) selected = (uint8_t)idx; }
  if (selected >= topIndex + VISIBLE_ROWS) topIndex = selected - VISIBLE_ROWS + 1;
  if (selected < topIndex) topIndex = selected;
}

void bumpPriorityAt(uint8_t idx, int8_t delta) {
  if (idx >= taskCount) return;
  int np = (int)tasks[idx].prio + (int)delta;
  if (np < PRI_LOW) np = PRI_LOW;
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

  const int16_t padX = 12, padY = 14, gapY = 12;
  int16_t gap = 12;
  int16_t optsRowW = (int16_t)wL + gap + (int16_t)wR;
  int16_t contentW = ((int16_t)wTitle > optsRowW) ? (int16_t)wTitle : optsRowW;
  int16_t bw = i16min((int16_t)(W - 2*8), (int16_t)(contentW + padX*2));
  int16_t innerW = bw - padX*2;
  if (optsRowW > innerW) {
    int16_t extra = optsRowW - innerW;
    int16_t newGap = gap - extra; if (newGap < 6) newGap = 6; gap = newGap;
  }

  uint16_t maxHL = (hL > hR ? hL : hR);
  int16_t bh = (int16_t)hTitle + gapY + (int16_t)maxHL + padY*2;
  int16_t bx = (W - bw) / 2;
  int16_t by = HEADER_HEIGHT + (LIST_CANVAS_H - bh) / 2;

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
// ---------- Brightness modal (centered) ----------
void drawBrightnessModal() {
  const char* title = "Brightness";
  uint8_t pct = g_blPct;
  if (pct < BL_MIN_PCT) pct = BL_MIN_PCT;
  if (pct > BL_MAX_PCT) pct = BL_MAX_PCT;
  uint8_t segs = (uint8_t)((uint32_t)(pct - BL_MIN_PCT) * 10u / (uint32_t)(BL_MAX_PCT - BL_MIN_PCT)); // 0..10
  if (segs > 10) segs = 10;

  char bar[20];
  uint8_t i=0;
  bar[i++]='[';
  for (uint8_t k=0;k<segs;k++) bar[i++]='=';
  for (uint8_t k=segs;k<10;k++) bar[i++]=' ';
  bar[i++]=']';
  bar[i]=' ';

  tft.setTextWrap(false);
  tft.setTextSize(2);

  int16_t x1,y1, xa,ya, xb,yb;
  uint16_t wTitle,hTitle, wMinus,hMinus, wPlus,hPlus, wBar,hBar;

  tft.getTextBounds(title, 0, 0, &x1, &y1, &wTitle, &hTitle);
  tft.getTextBounds("-", 0, 0, &xa, &ya, &wMinus, &hMinus);
  tft.getTextBounds("+", 0, 0, &xb, &yb, &wPlus, &hPlus);
  tft.getTextBounds(bar, 0, 0, &xb, &yb, &wBar, &hBar);

  const int16_t padX = 14, padY = 14, gapY = 12, gapX = 10;
  int16_t rowW = (int16_t)wMinus + gapX + (int16_t)wBar + gapX + (int16_t)wPlus;
  int16_t contentW = (wTitle > (uint16_t)rowW) ? (int16_t)wTitle : rowW;

  int16_t bw = (contentW + padX*2);
  if (bw > (W - 2*8)) bw = W - 2*8;
  int16_t innerW = bw - padX*2;

  int16_t usedGapX = gapX;
  if (rowW > innerW) {
    int16_t extra = rowW - innerW;
    int16_t tryGap = gapX - extra/2;
    if (tryGap < 6) tryGap = 6;
    usedGapX = tryGap;
    rowW = (int16_t)wMinus + usedGapX + (int16_t)wBar + usedGapX + (int16_t)wPlus;
  }

  uint16_t maxH = hBar;
  if (hMinus > maxH) maxH = hMinus;
  if (hPlus  > maxH) maxH = hPlus;
  int16_t bh = (int16_t)hTitle + gapY + (int16_t)maxH + padY*2;

  int16_t bx = (W - bw) / 2;
  int16_t by = HEADER_HEIGHT + (LIST_CANVAS_H - bh) / 2;

  tft.fillRoundRect(bx + 2, by + 3, bw, bh, 12, COL_SHADOW);
  uint16_t COL_PANEL = COL_MOVE;
  uint16_t COL_TXT   = COL_BLACK;
  tft.fillRoundRect(bx, by, bw, bh, 10, COL_PANEL);
  tft.drawRoundRect(bx, by, bw, bh, 10, COL_PANEL);

  int16_t tx = bx + (bw - (int16_t)wTitle)/2;
  int16_t ty = by + padY - y1;
  tft.setTextColor(COL_TXT, COL_PANEL);
  tft.setCursor(tx, ty);
  tft.print(title);

  int16_t rowX = bx + (bw - rowW)/2;
  int16_t rowY = ty + (int16_t)hTitle + gapY - ya;
  tft.setCursor(rowX, rowY);
  tft.print("-");

  int16_t barX = rowX + (int16_t)wMinus + usedGapX;
  tft.setCursor(barX, rowY - (yb - ya));
  tft.print(bar);

  int16_t plusX = barX + (int16_t)wBar + usedGapX;
  tft.setCursor(plusX, rowY - (yb - ya));
  tft.print("+");
}


// ---------- Read modal ----------
static void wrapTextToLines(const char* s, char out[][64], uint8_t &lines, uint8_t maxLines, uint8_t maxCharsPerLine) {
  lines = 0; if (!s || !*s) return;
  size_t N = strlen(s);
  char work[160]; size_t M = (N < sizeof(work)-1) ? N : (sizeof(work)-1);
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
    if (i < M && work[i] != ' ' && lastSpace != (size_t)-1 && lastSpace >= start) {
      end = lastSpace; i = lastSpace + 1;
    } else {
      end = i;
    }
    size_t L = end - start; if (L > 63) L = 63;
    memcpy(out[lines], &work[start], L); out[lines][L] = '\0'; lines++;
  }
}

void drawReadModal(uint8_t idx) {
  if (idx >= taskCount) return;
  tft.setTextWrap(false);
  tft.setTextSize(2);

  int16_t bxm,bym; uint16_t xW,xH; tft.getTextBounds("W", 0, 0, &bxm, &bym, &xW, &xH);
  if (xW == 0) xW = 8;

  const int16_t padX = 12, padY = 14;
  int16_t bw = W - 16;
  int16_t innerW = bw - padX*2;
  uint8_t maxCharsPerLine = innerW / (int16_t)xW;
  if (maxCharsPerLine < 6) maxCharsPerLine = 6;

  char lines[5][64]; uint8_t nLines = 0; wrapTextToLines(tasks[idx].text, lines, nLines, 5, maxCharsPerLine);
  if (nLines == 0) { strcpy(lines[0], "(empty)"); nLines = 1; }

  int16_t txb, tyb; uint16_t tw, th; tft.getTextBounds("X", 0, 0, &txb, &tyb, &tw, &th);
  int16_t textH = (int16_t)nLines * (int16_t)th;
  int16_t bh = padY*2 + textH; if (bh > LIST_CANVAS_H - 12) bh = LIST_CANVAS_H - 12;

  int16_t bx = (W - bw) / 2;
  int16_t by = HEADER_HEIGHT + (LIST_CANVAS_H - bh) / 2;

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
static inline void drainAllButtons(){ Btn::drain(bUp); Btn::drain(bDown); Btn::drain(bLeft); Btn::drain(bRight); Btn::drain(bOk); }

void enterMoveMode() {
  mode = MODE_MOVE; g_marqueeActive = false; markUIDirty(); rebuildHeader(true); rebuildListStatic();
}
void exitMoveMode(bool persist=true) {
  mode = MODE_NORMAL; if (persist) saveTasksRuntime();
  g_selStableSince = millis(); g_marqueeActive = false; markUIDirty(); rebuildHeader(true); rebuildListStatic();
}
void enterConfirmDelete(uint32_t uid) {
  mode = MODE_CONFIRM_DELETE; confirmDeleteUid = uid;
  g_okClicks = 0; g_okAnchorUid = 0; g_okLastClickMs = 0; g_marqueeActive = false;
  drainAllButtons(); confirmIgnoreUntil = millis() + 200;
  rebuildHeader(true); drawConfirmDelete();
}
void exitConfirmDelete(bool confirmed) {
  if (confirmed && confirmDeleteUid) deleteByUid(confirmDeleteUid);
  confirmDeleteUid = 0; mode = MODE_NORMAL;
  drainAllButtons(); inputSquelchUntil = millis() + 180;
  g_okClicks = 0; g_okAnchorUid = 0; g_selStableSince = millis(); g_marqueeActive = false; markUIDirty();
  rebuildHeader(true); rebuildListStatic();
}
void enterReadModal() { mode = MODE_READ; g_marqueeActive = false; g_okClicks = 0; g_okAnchorUid = 0; rebuildHeader(true); drawReadModal(selected); }
void exitReadModal()  { mode = MODE_NORMAL; drainAllButtons(); inputSquelchUntil = millis() + 180; g_selStableSince = millis(); g_marqueeActive = false; rebuildHeader(true); rebuildListStatic(); }

// --- Priority edit (LEFT/RIGHT hold in NORMAL) ---
void enterPrioEdit(int8_t dir) {
  if (taskCount == 0) return;
  mode = MODE_PRIO_EDIT; prioEditDir = dir; g_marqueeActive = false;
  bumpPriorityAt(selected, prioEditDir);
  g_prioNextBumpMs = millis() + PRIO_REPEAT_MS;
  rebuildHeader(true);
}
void exitPrioEdit() { mode = MODE_NORMAL; prioEditDir = 0; rebuildHeader(true); rebuildListStatic(); }

// ---------- Serial ----------
static uint8_t prioFromChar(char c) { if (c=='H'||c=='h') return PRI_HIGH; if (c=='M'||c=='m') return PRI_MED; return PRI_LOW; }
static char    prioToChar(uint8_t p) { if (p==PRI_HIGH) return 'H'; if (p==PRI_MED) return 'M'; return 'L'; }

void serialPrintHelp() {
  Serial.println(F("Commands:"));
  Serial.println(F(" HELP"));
  Serial.println(F(" LIST"));
  Serial.println(F(" EXPORT"));
  Serial.println(F(" IMPORT (then lines: TASK|H|0|Your text ; finish with END)"));
  Serial.println(F(" ADD <H|M|L> <text>"));
  Serial.println(F(" CLEAR"));
}
void serialList() {
  Serial.print(F("Count: ")); Serial.println(taskCount);
  for (uint8_t i=0;i<taskCount;i++) {
    Serial.print(i); Serial.print(F(": ")); Serial.print(prioToChar(tasks[i].prio));
    Serial.print(F(" | ")); Serial.print(tasks[i].done ? '1':'0'); Serial.print(F(" | ")); Serial.println(tasks[i].text);
  }
}
void serialExport() {
  Serial.println(F("BEGIN"));
  for (uint8_t i=0; i<taskCount;i++) {
    Serial.print(F("TASK|")); Serial.print(prioToChar(tasks[i].prio));
    Serial.print(F("|"));    Serial.print(tasks[i].done ? '1':'0');
    Serial.print(F("|"));    Serial.println(tasks[i].text);
  }
  Serial.println(F("END"));
}

void saveTasksRuntime() {
  prefs.begin("myday", false);
  prefs.putUChar("count", taskCount);
  for (uint8_t i = 0; i < taskCount; ++i) {
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

void serialClear() { taskCount = 0; saveTasksRuntime(); rebuildListStatic(); Serial.println(F("OK CLEARED")); }

void serialAdd(uint8_t pr, const char* text, bool done=false) {
  if (taskCount >= MAX_TASKS) { Serial.println(F("ERR FULL")); return; }
  Task t{};
  strncpy(t.text, text, sizeof(t.text)-1); t.text[sizeof(t.text)-1] = '\0';
  t.done = done; t.prio = pr; t.pend = PEND_NONE; t.pendStartMs=0; t.animStartMs=0; t.uid = nextUid++;
  tasks[taskCount++] = t;
  saveTasksRuntime();
  sortTasksByPriorityKeepSelection();
  rebuildListStatic();
  Serial.println(F("OK ADDED"));
}

void serialParseLine(char* line) {
  size_t n = strlen(line);
  while (n && (line[n-1]=='\r' || line[n-1]=='\n')) { line[n-1]=0; n--; }
  if (!n) return;
  if (g_serialImport) {
    if (strcmp(line, "END")==0) {
      g_serialImport = false; saveTasksRuntime(); sortTasksByPriorityKeepSelection(); rebuildListStatic();
      Serial.println(F("OK IMPORT END")); return;
    }
    if (strncmp(line, "TASK|", 5)==0) {
      char* p = line + 5; if (!*p) { Serial.println(F("ERR FORMAT")); return; }
      uint8_t pr = prioFromChar(*p); p++;
      if (*p!='|') { Serial.println(F("ERR FORMAT")); return; } p++;
      if (!*p)     { Serial.println(F("ERR FORMAT")); return; }
      bool done = (*p=='1');
      while (*p && *p!='|') p++;
      if (*p!='|') { Serial.println(F("ERR FORMAT")); return; }
      p++; serialAdd(pr, p, done); return;
    } else {
      Serial.println(F("ERR EXPECT TASK|H|0|text or END")); return;
    }
  }
  if (strcmp(line,"HELP")==0)   { serialPrintHelp(); return; }
  if (strcmp(line,"LIST")==0)   { serialList(); return; }
  if (strcmp(line,"EXPORT")==0) { serialExport(); return; }
  if (strcmp(line,"IMPORT")==0) { g_serialImport=true; Serial.println(F("OK IMPORT BEGIN")); return; }
  if (strcmp(line,"CLEAR")==0)  { serialClear(); return; }
  if (strncmp(line,"ADD ",4)==0) {
    char* p = line + 4; if (!*p) { Serial.println(F("ERR ADD")); return; }
    uint8_t pr = PRI_LOW;
    if (*p=='H'||*p=='h'||*p=='M'||*p=='m'||*p=='L'||*p=='l') {
      pr = prioFromChar(*p); p++; if (*p==' ') p++;
    }
    if (!*p) { Serial.println(F("ERR ADD TEXT")); return; }
    serialAdd(pr, p, false); return;
  }
  Serial.println(F("ERR UNKNOWN. Type HELP."));
}

void serialService() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      g_lineBuf[g_lineLen] = '\0'; serialParseLine(g_lineBuf); g_lineLen = 0;
    } else {
      if (g_lineLen < sizeof(g_lineBuf) - 1) g_lineBuf[g_lineLen++] = c;
      else g_lineBuf[sizeof(g_lineBuf) - 1] = '\0';
    }
  }
}

// ---------- Arduino ----------
void setup() {
#if USE_HARDWARE_SPI
  #if defined(ESP32)
    SPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);
  #else
    SPI.begin();
  #endif
#endif

  setupBacklightPwm();

  tft.init(170, 320);
  tft.setRotation(3);
  tft.setOffset(35, 0); // swap to (0,35) for alternate panels
  tft.setSPISpeed(SPI_SPEED_HZ);

  // Color palette (with soft navy BG from BG_COL.h)
  COL_BG         = BG_COLOR565; // from your BG PNG
  COL_TEXT       = tft.color565(0xC6, 0xD0, 0xF5);
  COL_DIM        = tft.color565(0x83, 0x8B, 0xA7);
  COL_ACCENT     = tft.color565(0x85, 0xC1, 0xDC);
  COL_FRAME      = tft.color565(0x51, 0x57, 0x6D);
  COL_BORDER     = tft.color565(0x73, 0x79, 0x94);
  COL_HIL        = tft.color565(0x8C, 0xAA, 0xEE);
  COL_FOOT       = tft.color565(0x99, 0xD1, 0xDB);
  COL_BLACK      = tft.color565(0x00, 0x00, 0x00);
  COL_MODAL_PANEL= tft.color565(0xF2, 0xD5, 0xCF);
  COL_MOVE       = tft.color565(0xEE, 0xD4, 0x9F);
  COL_WHITE      = tft.color565(0xFF, 0xFF, 0xFF);
  COL_X_HARD     = tft.color565(0xA6, 0xD1, 0x89);
  COL_PRI_HIGH   = tft.color565(0xE7, 0x82, 0x84);
  COL_PRI_MED    = tft.color565(0xEF, 0x9F, 0x76);
  COL_CLOCK      = tft.color565(0x89, 0xDC, 0xEB);
  COL_NAV_BAR    = tft.color565(0x5A, 0x66, 0xA6); // lighter variant of panel color
  // Nav icon palette: active colors as requested, with grey for inactive
  COL_NAV_ICON0  = tft.color565(0xA6, 0xDA, 0x95); // #a6da95
  COL_NAV_ICON1  = tft.color565(0x8A, 0xAD, 0xF4); // #8aadf4
  COL_NAV_ICON2  = tft.color565(0xEE, 0xD4, 0x9F); // #eed49f
  COL_NAV_ICON3  = tft.color565(0xF5, 0xBD, 0xE6); // #f5bde6
  COL_ICON_GREY  = tft.color565(0x83, 0x8B, 0xA7); // grey
  COL_BOLT       = tft.color565(0x8A, 0xAD, 0xF4); // bolt heads (blue)
  COL_TAB_HIL    = tft.color565(0xA6, 0xD1, 0x89); // #a6d189
  COL_NAV_SEL    = tft.color565(0xC6, 0xD0, 0xF5); // #c6d0f5
  COL_DATE_TXT   = tft.color565(0xB8, 0xC0, 0xE0); // #b8c0e0
  COL_SAVER_PILL = tft.color565(0xF5, 0xA9, 0x7F); // #f5a97f

  tft.fillScreen(COL_BG);
  COL_SHADOW = fade565(COL_BLACK, COL_BG, 80);

  Btn::init(bUp);
  Btn::init(bOk);
  Btn::init(bDown);
  Btn::init(bLeft);
  Btn::init(bRight);

  Serial.begin(SERIAL_BAUD);

  applySeedIfChangedAndLoad();
  loadUIState();

  applyBacklight();              // apply loaded brightness
  g_blOsdUntil = 0;              // do not show OSD on boot

  mode = MODE_NORMAL;
  sortTasksByPriorityKeepSelection();

  clockStartMs = millis();
  g_selStableSince = clockStartMs;
  g_marqueeActive = false;

  rebuildHeader(true);
  rebuildListStatic();
}

void loop() {
  uint32_t now = millis();
  serialService();

  Btn::update(bUp);
  Btn::update(bDown);
  Btn::update(bLeft);
  Btn::update(bRight);
  Btn::update(bOk);

  // Track user activity to control screensaver
  bool anyEdge = Btn::pressed(bUp) || Btn::pressed(bDown) || Btn::pressed(bLeft) || Btn::pressed(bRight) || Btn::pressed(bOk)
              || Btn::released(bUp) || Btn::released(bDown) || Btn::released(bLeft) || Btn::released(bRight) || Btn::released(bOk)
              || Btn::clicked(bUp) || Btn::clicked(bDown) || Btn::clicked(bLeft) || Btn::clicked(bRight) || Btn::clicked(bOk);
  if (anyEdge) g_lastUserInputMs = now;

  bool inputsReady = ((int32_t)(now - inputSquelchUntil) >= 0);

  // Long-hold OK to read
  if (mode == MODE_NORMAL && inputsReady && Btn::heldFor(bOk, READ_HOLD_MS)) {
    if (taskCount > 0) enterReadModal();
  }

  // ===== MODE handlers =====
  // Enter screensaver on idle
  if (mode != MODE_SCREENSAVER && (uint32_t)(now - g_lastUserInputMs) >= SCREENSAVER_IDLE_MS) {
    mode = MODE_SCREENSAVER; g_saverAnimStartMs = now; g_mockBatteryPhase = 0; rebuildHeader(true);
  }
  // Wake from screensaver on any input
  if (mode == MODE_SCREENSAVER && anyEdge) {
    mode = MODE_NORMAL; g_lastUserInputMs = now; rebuildHeader(true); rebuildListStatic();
  }
  if (mode == MODE_SCREENSAVER) {
    // Draw screensaver frame and return
    // Use off-screen canvases to avoid flicker
    GFXcanvas16 saver(W, H);
    saver.fillScreen(COL_BG);
    // Time large + date smaller
    char timeStr[16];
    uint32_t elapsedMs = millis() - clockStartMs;
    uint32_t totalMin = startHour * 60UL + startMin + (elapsedMs / 60000UL);
    uint8_t hh = (totalMin / 60U) % 24U;
    uint8_t mm = (uint8_t)(totalMin % 60U);
    bool pm = (hh >= 12); uint8_t h12 = hh % 12; if (h12 == 0) h12 = 12;
    snprintf(timeStr, sizeof(timeStr), "%u:%02u %s", h12, mm, pm?"PM":"AM");
    // Center time
    saver.setTextWrap(false);
    saver.setTextSize(3);
    int16_t bx, by; uint16_t tw, th;
    saver.getTextBounds(timeStr, 0, 0, &bx, &by, &tw, &th);
    int16_t tx = (W - (int16_t)tw) / 2;
    int16_t ty = HEADER_HEIGHT + (LIST_CANVAS_H/2) - th - 6;
    saver.setTextColor(COL_WHITE, COL_BG);
    saver.setCursor(tx, ty); saver.print(timeStr);
    // Mock date below (fixed string)
    const char* dateStr = "Thu 08/22";
    saver.setTextSize(2);
    int16_t dx, dy; uint16_t dw, dh;
    saver.getTextBounds(dateStr, 0, 0, &dx, &dy, &dw, &dh);
    int16_t dtx = (W - (int16_t)dw) / 2;
    int16_t dty = ty + th + 10 - dy;
    // Orange pill behind date
    int16_t pillPadX = 10, pillPadY = 4;
    int16_t pdw = (int16_t)dw + pillPadX*2;
    int16_t pdh = (int16_t)dh + pillPadY*2;
    saver.fillRoundRect(dtx - pillPadX, dty - pillPadY, pdw, pdh, 4, COL_SAVER_PILL);
    saver.setCursor(dtx, dty); saver.setTextColor(COL_WHITE, COL_SAVER_PILL); saver.print(dateStr);
    // Bottom-left remaining tasks (text only, flush-left)
    uint16_t remaining=0, completed=0; for (uint8_t i=0;i<taskCount;i++){ if (tasks[i].done) completed++; else remaining++; }
    uint16_t COL_LEFT = tft.color565(0x91,0xD7,0xE3); // #91d7e3
    saver.setTextSize(2);
    saver.setCursor(0, H - 6 - 16);
    saver.setTextColor(COL_WHITE, COL_BG); saver.print("[ ");
    saver.setTextColor(COL_LEFT,  COL_BG); saver.print(remaining);
    saver.setTextColor(COL_WHITE, COL_BG); saver.print(" ]");
    // Top-right 8-bit battery
    uint16_t batCol = (g_mockBatteryPhase==0)? tft.color565(0xE7,0x82,0x84) : (g_mockBatteryPhase==1? tft.color565(0xEF,0x9F,0x76) : tft.color565(0xA6,0xDA,0x95));
    int16_t bx0 = W - 6 - 28;
    int16_t by0 = 6;
    int16_t bw  = 28, bh = 14;
    // 8-bit chunky outline (thicker)
    for (int8_t o = 0; o < 3; ++o) {
      saver.drawRect(bx0 - o, by0 - o, bw + 2*o, bh + 2*o, COL_WHITE);
    }
    saver.fillRect(bx0 + bw, by0 + (bh/2 - 2), 3, 4, COL_WHITE);
    // Pixel bars
    for (uint8_t k=0;k<4;k++) {
      int16_t segW = 5, segH = 8, gap=2;
      int16_t sx = bx0 + 2 + k*(segW + gap);
      int16_t sy = by0 + 3;
      saver.fillRect(sx, sy, segW, segH, batCol);
    }
    // cycle battery color every 1.2s
    if ((uint32_t)(now - g_saverAnimStartMs) >= 1200) { g_saverAnimStartMs = now; g_mockBatteryPhase = (uint8_t)((g_mockBatteryPhase + 1) % 3); }
    // Blit once
    tft.drawRGBBitmap(0, 0, saver.getBuffer(), W, H);
    return;
  }
  if (mode == MODE_MOVE && inputsReady) {
    if (Btn::pressed(bUp) || Btn::repeat(bUp))   moveSelectedRow(-1);
    if (Btn::pressed(bDown) || Btn::repeat(bDown)) moveSelectedRow(+1);

    // Brightness while in MOVE
    if (Btn::pressed(bLeft) || Btn::repeat(bLeft)) {
if (g_blPct > BL_MIN_PCT) setBacklightPct(g_blPct - BL_STEP, true);
  rebuildHeader(true);
  rebuildListStatic();
}
    if (Btn::pressed(bRight) || Btn::repeat(bRight)) {
if (g_blPct < BL_MAX_PCT) setBacklightPct(g_blPct + BL_STEP, true);
  rebuildHeader(true);
  rebuildListStatic();
}
  } else if (mode == MODE_CONFIRM_DELETE) {
    bool gateOpen = ( (int32_t)(now - confirmIgnoreUntil) >= 0 ) && !bUp.debPressed && !bDown.debPressed && !bOk.debPressed && !bLeft.debPressed && !bRight.debPressed;
    if (gateOpen) {
      if (Btn::clicked(bUp))   { exitConfirmDelete(true); }
      if (Btn::clicked(bDown)) { exitConfirmDelete(false); }
    }
  } else if (mode == MODE_READ) {
    if (!bOk.debPressed && Btn::released(bOk)) exitReadModal();
  } else if (mode == MODE_PRIO_EDIT) {
    if (prioEditDir > 0) {
      if (!bRight.debPressed) { exitPrioEdit(); }
      else if ((int32_t)(now - g_prioNextBumpMs) >= 0) { bumpPriorityAt(selected, +1); g_prioNextBumpMs = now + PRIO_REPEAT_MS; }
    } else if (prioEditDir < 0) {
      if (!bLeft.debPressed) { exitPrioEdit(); }
      else if ((int32_t)(now - g_prioNextBumpMs) >= 0) { bumpPriorityAt(selected, -1); g_prioNextBumpMs = now + PRIO_REPEAT_MS; }
    }
  } else if (mode == MODE_NAV && inputsReady) {
    // Navigate between 4 icons using UP/DOWN; exit with double LEFT again or single RIGHT
    if (Btn::pressed(bUp) || Btn::repeat(bUp)) {
      if (navIndex > 0) navIndex--;
      if (navIndex < navScrollTop) navScrollTop = navIndex;
      rebuildHeader(true); rebuildListStatic();
    }
    if (Btn::pressed(bDown) || Btn::repeat(bDown)) {
      if (navIndex + 1 < NAV_TOTAL) navIndex++;
      if (navIndex >= navScrollTop + NAV_VISIBLE) navScrollTop = navIndex - NAV_VISIBLE + 1;
      rebuildHeader(true); rebuildListStatic();
    }
    // Double-tap LEFT to exit NAV
    if (Btn::clicked(bLeft)) {
      if (g_leftClicks == 0) { g_leftClicks = 1; g_leftLastClickMs = now; }
      else if (now - g_leftLastClickMs <= MULTI_MS) {
        // Second tap within window: exit NAV
        mode = MODE_NORMAL; g_leftClicks = 0; g_leftLastClickMs = 0;
        rebuildHeader(true); rebuildListStatic();
      } else { g_leftClicks = 1; g_leftLastClickMs = now; }
    }
    if (Btn::clicked(bRight)) { mode = MODE_NORMAL; rebuildHeader(true); rebuildListStatic(); }
  } else if (mode == MODE_NORMAL && inputsReady) {
    // Enter priority editor with LEFT/RIGHT holds
    bool enteredPrio = false;
    if (Btn::holdStart(bRight)) { enterPrioEdit(+1); enteredPrio = true; }
    else if (Btn::holdStart(bLeft)) { enterPrioEdit(-1); enteredPrio = true; }

    if (!enteredPrio) {
      // Compute visible offset once for UP handling
      int visOff = visibleOffsetFromTop(topIndex, selected);
      // Focus header when at top-of-window; otherwise let UP move selection
      if (!headerFocused && Btn::released(bUp) && Btn::lastPressDurationMs(bUp) < HOLD_START_MS && visOff == 0) {
        headerFocused = true; rebuildHeader(true);
      } else if (headerFocused) {
        // Toggle page with RIGHT/LEFT; DOWN returns to list
        if (Btn::released(bRight) && Btn::lastPressDurationMs(bRight) < HOLD_START_MS) {
          currentPage = TAB_COMPLETED; ensureSelectionValidForPage(); rebuildHeader(true); rebuildListStatic();
        }
        if (Btn::released(bLeft)  && Btn::lastPressDurationMs(bLeft)  < HOLD_START_MS) {
          currentPage = TAB_TASKS;     ensureSelectionValidForPage(); rebuildHeader(true); rebuildListStatic();
        }
        if (Btn::released(bDown)  && Btn::lastPressDurationMs(bDown)  < HOLD_START_MS) { headerFocused = false; rebuildHeader(true); }
      } else {
        if ((Btn::pressed(bUp) || Btn::repeat(bUp)) && Btn::lastPressDurationMs(bUp) < HOLD_START_MS) {
          // Prefer moving to previous visible; if none, focus header
          int prev = findNextVisible((int)selected, -1);
          if (prev >= 0) ensureSelectionVisibleAndDraw(-1);
          else { headerFocused = true; rebuildHeader(true); }
        }
        if ((Btn::pressed(bDown) || Btn::repeat(bDown)) && Btn::lastPressDurationMs(bDown) < HOLD_START_MS) ensureSelectionVisibleAndDraw(+1);
      }
      // Double-tap LEFT to enter NAV in NORMAL
      if (Btn::clicked(bLeft)) {
        if (g_leftClicks == 0) { g_leftClicks = 1; g_leftLastClickMs = now; }
        else if (now - g_leftLastClickMs <= MULTI_MS) {
          // Enter NAV
          mode = MODE_NAV; g_leftClicks = 0; g_leftLastClickMs = 0;
          // Ensure current navIndex is within window
          if (navIndex < navScrollTop) navScrollTop = navIndex;
          if (navIndex >= navScrollTop + NAV_VISIBLE) navScrollTop = navIndex - NAV_VISIBLE + 1;
          rebuildHeader(true); rebuildListStatic();
        } else { g_leftClicks = 1; g_leftLastClickMs = now; }
      }
    }
  }

  // OK multi-clicks (not in confirm/read/prio/nav)
  if (mode != MODE_CONFIRM_DELETE && mode != MODE_READ && mode != MODE_PRIO_EDIT && mode != MODE_NAV && inputsReady && Btn::clicked(bOk)) {
    if (g_okClicks == 0) {
      g_okClicks = 1; g_okLastClickMs = now;
      g_okAnchorUid = (selected < taskCount) ? tasks[selected].uid : 0;
    } else if (now - g_okLastClickMs <= MULTI_MS) {
      g_okClicks++; g_okLastClickMs = now;
      if (g_okClicks >= 3) { enterConfirmDelete(g_okAnchorUid); g_okClicks = 0; g_okAnchorUid = 0; }
    } else {
      // Timing window expired; start a fresh single-click sequence
      g_okClicks = 1; g_okLastClickMs = now;
      g_okAnchorUid = (selected < taskCount) ? tasks[selected].uid : 0;
    }
  }

  // Resolve single/double after window
  if (mode != MODE_CONFIRM_DELETE && mode != MODE_READ && mode != MODE_PRIO_EDIT && mode != MODE_NAV && g_okClicks > 0 && (now - g_okLastClickMs > MULTI_MS)) {
    uint8_t n = g_okClicks; g_okClicks = 0;
    if (mode == MODE_MOVE) {
      if (n == 2) exitMoveMode(true);
    } else {
      if (n == 1) {
        if (g_okAnchorUid) {
          // Prevent toggling if current selection is hidden in this tab
          int idx = findIndexByUid(g_okAnchorUid);
          if (idx >= 0 && isVisibleInCurrentPage((uint8_t)idx)) toggleByUid(g_okAnchorUid);
        }
      }
      else if (n == 2) { if (mode == MODE_NORMAL) enterMoveMode(); else exitMoveMode(true); }
    }
    g_okAnchorUid = 0;
  }

  // Resolve LEFT double-tap window when in modes where we collect LEFT clicks
  if ((mode == MODE_NORMAL || mode == MODE_NAV) && g_leftClicks > 0 && (now - g_leftLastClickMs > MULTI_MS)) {
    // Single LEFT tap does nothing in NORMAL; in NAV we keep state
    g_leftClicks = 0;
  }

  // Pending pipeline
  // Disabled delete/complete animation

  // Auto marquee
  if (mode == MODE_NORMAL && taskCount > 0) {
    int16_t textX, availW; computeTextArea(textX, availW);
    int16_t bx, by; uint16_t tw, th; listCanvas.setTextSize(2); listCanvas.getTextBounds(tasks[selected].text, 0, 0, &bx, &by, &tw, &th);
    bool tooLong = (tw > (uint16_t)availW);
    if (tooLong) {
      if (!g_marqueeActive) {
        if ((uint32_t)(now - g_selStableSince) >= MARQUEE_IDLE_MS) {
          g_marqueeActive = true; g_marqueeStartMs = now; g_lastMarqueeFrame = 0;
        }
      }
      if (g_marqueeActive) {
        uint32_t elapsed = now - g_marqueeStartMs;
        uint32_t cycleW = (uint32_t)tw + (uint32_t)MARQUEE_GAP_PX;
        uint32_t px = (elapsed * MARQUEE_SPEED_PX_PER_S) / 1000U;
        if (cycleW == 0) cycleW = 1;
        g_marqueeOffsetPx = (int16_t)(px % cycleW);
        if ((uint32_t)(now - g_lastMarqueeFrame) >= MARQUEE_FRAME_MS) {
          rebuildListStatic(); g_lastMarqueeFrame = now;
        }
      }
    } else g_marqueeActive = false;
  } else g_marqueeActive = false;

  // Debounced saves (UI + brightness)
  if (uiDirty && (now - lastUISaveMs) >= UI_SAVE_DEBOUNCE_MS) saveUIState();
  if (g_blDirty && (now - g_blLastSaveMs) >= 600) {
    prefs.begin("myday", false);
    prefs.putUChar("bl_pct", g_blPct);
    prefs.end();
    g_blDirty = false;
  }

  rebuildHeader(false);
}
