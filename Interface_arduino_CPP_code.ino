/*******************************************************
 * CNC Jog UI 480x320 ST7796 + XPT2046
 * - Keep your pages and layout
 * - Positions fed by big code (handleTEENSYLine) via extern lastPosition*
 * - Remove local parsing / polling of GRBL
 * - Stable touch: pressed color while held, single fire
 * - Send via TEENSYSerialWrite(...)
 *******************************************************/
#include <Arduino.h>
#include <SPI.h>

// Touch controller pins (XPT2046)
#define TOUCH_CS   15
#define TOUCH_IRQ  -1

#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <math.h>

/* ================= Instances ====================== */
TFT_eSPI tft = TFT_eSPI();
XPT2046_Touchscreen ts(TOUCH_CS, TOUCH_IRQ);
TFT_eSprite pageSpr = TFT_eSprite(&tft);  // Off-screen framebuffer
bool useSprite = true;                    // Fallback if RAM is tight


/* ================= Config ========================= */
#define ROTATION   1
#define SWAP_XY    0

// XPT2046 raw calibration (tune if needed)
static const int16_t RAW_MIN_X = 230;
static const int16_t RAW_MAX_X = 3930;
static const int16_t RAW_MIN_Y = 190;
static const int16_t RAW_MAX_Y = 3850;

/* ================= Colors ========================= */
static const uint16_t COL_BG         = 0xFFFF;
static const uint16_t COL_PANEL      = 0xE71C;
static const uint16_t COL_FRAME      = 0x8410;
static const uint16_t COL_TEXT       = 0x0000;
static const uint16_t COL_MUTED      = 0xAD55;
static const uint16_t COL_ACCENT     = 0x059F;
static const uint16_t COL_WARN       = 0xFBE0;
static const uint16_t COL_PRESSGLOW  = 0xC618;

static const uint16_t COL_X = 0xF81F;
static const uint16_t COL_Y = 0x07FF;
static const uint16_t COL_Z = 0x07E0;



/* ================= Layout ========================= */
// Screen: 480x320 at ROTATION=1
static int16_t W, H;

static const int16_t HEADER_Y     = 8;   // all column titles drawn here (same level)
static const int16_t WORK_TOP_Y   = 24;  // top of panels
static const int16_t BAR_H        = 44;
static const int16_t BTN_R        = 10;
static const int16_t GAP          = 10;

// Left XY pad (slimmed to make room)
static const int16_t PAD_X  = 8;
static const int16_t PAD_W  = 160;
static const int16_t PAD_Y  = WORK_TOP_Y;
static const int16_t PAD_H  = 245;

// Z column (narrow)
static const int16_t ZCOL_X = PAD_X + PAD_W + GAP;
static const int16_t ZCOL_W = 70;
static const int16_t ZCOL_Y = WORK_TOP_Y;
static const int16_t ZCOL_H = PAD_H;

// STEP column (right 1)
static const int16_t STEP_X = ZCOL_X + ZCOL_W + GAP;
static const int16_t STEP_W = 100;
static const int16_t STEP_Y = WORK_TOP_Y;
static const int16_t STEP_H = PAD_H;

// FEED column (right 2)
static const int16_t FEED_X = STEP_X + STEP_W + GAP;
static const int16_t FEED_W = 100;
static const int16_t FEED_Y = WORK_TOP_Y;
static const int16_t FEED_H = PAD_H;

// Page toggle button (computed after W/H known)
static const int16_t PAGE_BTN_SIZE = 40;
static int16_t PAGE_BTN_X = 0;
static int16_t PAGE_BTN_Y = 0;

/* ===== Page-2 log panel (shrink to 250x100) ===== */
static const int16_t LOG_W = 250;
static const int16_t LOG_H = 100;           // was 200 → now 100
static const int16_t LOG_X = 10;
static const int16_t LOG_Y = WORK_TOP_Y - 12;

static const uint8_t LOG_MAX_LINES = 6;   
String logLines[LOG_MAX_LINES];
uint8_t logCount = 0;

static const int INP_W = LOG_W;
static const int INP_H = 50;
static const int INP_X = LOG_X;
static const int INP_Y = 5 + LOG_Y + LOG_H; // below logger

/* ===== Always-on position boxes (MPos / WPos) ===== */
static const int16_t POS_BOX_W   = 250;
static const int16_t POS_BOX_H   = 46;
static const int16_t MPOS_BOX_X  = 10;              // right side
static const int16_t MPOS_BOX_Y  = 5 + INP_Y + INP_H;
static const int16_t WPOS_BOX_X  = 10;
static const int16_t WPOS_BOX_Y  = MPOS_BOX_Y + POS_BOX_H + 5;

/* ===== Status button geometry (next to Page button) ===== */
// We’ll draw the box for aesthetics, but we’re not updating state here
static const int16_t STATUS_BTN_W = 100;
static const int16_t STATUS_BTN_H = PAGE_BTN_SIZE;
static int16_t STATUS_BTN_X = 0;
static int16_t STATUS_BTN_Y = 0;

/* ================= Buttons ======================== */
struct Button {
  int16_t x, y, w, h;
  uint16_t baseColor;
  const char* label;
  uint8_t id;
  bool momentary;   // true: action; false: selection
  bool selected;    // for selection groups
};

// Main page button IDs
enum BtnId : uint8_t {
  B_XM, B_XP, B_YM, B_YP, B_XY0,
  B_ZP, B_Z0, B_ZM,
  B_S100, B_S10, B_S1, B_S01,          // Step group (vertical)
  B_F2000, B_F1000, B_F300, B_F100,    // Feed group (vertical)
  B_HOME,
  BTN_COUNT
};
Button btns[BTN_COUNT];

uint8_t selStep = B_S10;
uint8_t selFeed = B_F1000;

// -------- ALT PAGE (keypad) ----------
enum AltBtnId : uint8_t {
  // keypad: row-major order
  A_K7, A_K8, A_K9, A_KQMARK,   // 7 8 9 ?
  A_K4, A_K5, A_K6, A_KDOLLAR,  // 4 5 6 $
  A_K1, A_K2, A_K3, A_KG,       // 1 2 3 G
  A_K0, A_KDOT, A_KMINUS, A_KF, // 0 . - F
  A_KSPACE, A_X, A_Y, A_Z,      // space, X, Y, Z
  A_BACK, A_ENTER,              // Main, Enter
  A_DEL, A_EQUAL,
  ALT_BTN_COUNT
};
Button altBtns[ALT_BTN_COUNT];

/* ================= Page state ====================== */
enum PageId : uint8_t { PAGE_MAIN = 0, PAGE_ALT = 1 };
uint8_t currentPage = PAGE_MAIN;
bool needsRedraw = true;

/* ================= Steps/Feeds ===================== */
static inline float stepValueFor(uint8_t id) {
  switch (id) {
    case B_S100: return 100.f;
    case B_S10:  return 10.f;
    case B_S1:   return 1.f;
    case B_S01:  return 0.1f;
    default:     return 10.f;
  }
}
static inline int feedValueFor(uint8_t id) {
  switch (id) {
    case B_F2000: return 2000;
    case B_F1000: return 1000;
    case B_F300:  return 300;
    case B_F100:  return 100;
    default:      return 1000;
  }
}

/* ================= Simple status pill (static) ===== */
static inline void drawStatusButton() {
  // Draw a neutral pill; content not live-updated here
  tft.fillRoundRect(STATUS_BTN_X, STATUS_BTN_Y, STATUS_BTN_W, STATUS_BTN_H, 10, COL_PANEL);
  tft.drawRoundRect(STATUS_BTN_X, STATUS_BTN_Y, STATUS_BTN_W, STATUS_BTN_H, 10, COL_FRAME);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(COL_TEXT, COL_PANEL);
  tft.setTextFont(1);
  tft.setTextSize(2);
  tft.drawCentreString("GRBL", STATUS_BTN_X + STATUS_BTN_W/2, STATUS_BTN_Y + STATUS_BTN_H/2, 1);
  tft.setTextSize(1);
}

/* ============== MPos / WPos boxes ================= */
static inline void drawPosBoxFrame(int x, int y, const char* title) {
  tft.fillRoundRect(x, y, POS_BOX_W, POS_BOX_H, 8, COL_BG);
  tft.drawRoundRect(x, y, POS_BOX_W, POS_BOX_H, 8, COL_FRAME);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(COL_MUTED, COL_BG);
  tft.drawString(title, x + 8, y + 4);
}

/* ============== Headers / Panels ================== */
void drawHeadersSameLine() {
  tft.setTextColor(COL_MUTED, COL_BG);
  tft.setTextDatum(MC_DATUM);
  int16_t cx_xy   = PAD_X + PAD_W/2;
  int16_t cx_z    = ZCOL_X + ZCOL_W/2;
  int16_t cx_step = STEP_X + STEP_W/2;
  int16_t cx_feed = FEED_X + FEED_W/2;
  tft.drawCentreString("X / Y",         cx_xy,   HEADER_Y, 1);
  tft.drawCentreString("Z",             cx_z,    HEADER_Y, 1);
  tft.drawCentreString("STEP (mm)",     cx_step, HEADER_Y, 1);
  tft.drawCentreString("FEED (mm/min)", cx_feed, HEADER_Y, 1);
}

void drawPanelFrames() {
  tft.fillScreen(COL_BG);

  auto drawPanel = [&](int16_t x,int16_t y,int16_t w,int16_t h){
    tft.fillRoundRect(x-4, y-4, w+8, h+8, 10, COL_PANEL);
    tft.drawRoundRect(x-4, y-4, w+8, h+8, 10, COL_FRAME);
  };

  if (currentPage == PAGE_MAIN) {
    drawHeadersSameLine();
    drawPanel(PAD_X,  PAD_Y,  PAD_W,  PAD_H);     // XY
    drawPanel(ZCOL_X, ZCOL_Y, ZCOL_W, ZCOL_H);    // Z
    drawPanel(STEP_X, STEP_Y, STEP_W, STEP_H);    // STEP
    drawPanel(FEED_X, FEED_Y, FEED_W, FEED_H);    // FEED
  } else {
    // Simpler background for ALT page
    tft.fillRoundRect(6, 4, W-12, H  - BAR_H - 6, 10, COL_PANEL);
    tft.drawRoundRect(6, 4, W-12, H  - BAR_H - 6, 10, COL_FRAME);
  }

  // bottom bar
  tft.fillRect(0, H-BAR_H, W, BAR_H, COL_PANEL);
  tft.drawLine(0, H-BAR_H, W, H-BAR_H, COL_FRAME);

  // draw 80x40 Page button (double width)
  tft.fillRoundRect(PAGE_BTN_X, PAGE_BTN_Y, PAGE_BTN_SIZE*2, PAGE_BTN_SIZE, 10, COL_ACCENT);
  tft.drawRoundRect(PAGE_BTN_X, PAGE_BTN_Y, PAGE_BTN_SIZE*2, PAGE_BTN_SIZE, 10, COL_FRAME);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(COL_TEXT, COL_ACCENT);
  tft.drawCentreString((currentPage==PAGE_MAIN) ? "P2" : "P1", PAGE_BTN_X + PAGE_BTN_SIZE, PAGE_BTN_Y + PAGE_BTN_SIZE/2, 1);

  // status pill (static look)
  drawStatusButton();
}

/* ================= Buttons draw =================== */
void drawButton(const Button& b, bool pressed=false) {
  uint16_t face = b.baseColor;
  uint16_t border = COL_FRAME;

  // selection (non-momentary) stays accented when selected
  if (!b.momentary && b.selected) { face = COL_ACCENT; border = COL_TEXT; }

  // momentary pressed → persistent pressed color (no blink)
  if (b.momentary && pressed) face = COL_ACCENT;

  // soft shadow
  tft.fillRoundRect(b.x + (pressed?2:3), b.y + (pressed?3:4), b.w, b.h, BTN_R, COL_PRESSGLOW);
  // face
  tft.fillRoundRect(b.x, b.y, b.w, b.h, BTN_R, face);
  tft.drawRoundRect(b.x, b.y, b.w, b.h, BTN_R, border);

  // big label
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(COL_TEXT, face);
  tft.setTextFont(1);
  tft.setTextSize(2);
  tft.drawCentreString(b.label, b.x + b.w/2, b.y + b.h/2, 1);
  tft.setTextSize(1);
}

void refreshSelections() {
  if (currentPage != PAGE_MAIN) return;
  for (auto &b : btns) if (!b.momentary) drawButton(b);
}

/* ================= Hit-test ======================= */
int8_t hitTestMain(int16_t x, int16_t y) {
  for (uint8_t i=0; i<BTN_COUNT; ++i) {
    auto &b = btns[i];
    if (x>=b.x && x<b.x+b.w && y>=b.y && y<b.y+b.h) return i;
  }
  return -1;
}
int8_t hitTestAlt(int16_t x, int16_t y) {
  for (uint8_t i=0; i<ALT_BTN_COUNT; ++i) {
    auto &b = altBtns[i];
    if (x>=b.x && x<b.x+b.w && y>=b.y && y<b.y+b.h) return i;
  }
  return -1;
}

/* ============== G-code printing =================== */
static inline void grblSendChar(char c) {}
static inline void grblSend(const String& s) { for (size_t i=0;i<s.length();++i) grblSendChar(s[i]); }
static inline void grblSendLine(const String& s) { grblSend(s); grblSendChar('\n'); }

void gcodeJog(char axis, float dist, int feed) {
  grblSendLine("G91"); // relative
  String cmd = "G1 ";
  cmd += axis;
  if (dist >= 0) cmd += '+';
  cmd += String(dist, (fabs(dist - (int)dist) < 0.001f) ? 0 : 3);
  cmd += " F";
  cmd += feed;
  grblSendLine(cmd);
  grblSendLine("G90"); // back absolute
}
void gcodeGoAxisZero(char axis, int feed) {
  String cmd = "G92 ";
  cmd += axis; cmd += '0';
  cmd += " F"; cmd += feed;
  grblSendLine(cmd);
}
void gcodeGoXYZero(int feed) {
  String cmd = "G92 X0 Y0 F"; cmd += feed;
  grblSendLine(cmd);
}
void gcodeHome() { grblSendLine("$H"); }

/* ============== Touch / input ===================== */
uint32_t lastTouchMs = 0;
const uint16_t TOUCH_DEBOUNCE_MS = 70;

static bool     touchDown   = false;
static int8_t   activeBtnId = -1;
static int8_t   activeAltId = -1;
static bool     actionSent  = false;

bool readTouch(int16_t &sx, int16_t &sy) {
  if (!ts.touched()) return false;
  TS_Point p = ts.getPoint();
  int16_t rx = p.x, ry = p.y;
  if (SWAP_XY) { int16_t tmp = rx; rx = ry; ry = tmp; }
  sx = map(rx, RAW_MAX_X, RAW_MIN_X, 0, W);
  sy = map(ry, RAW_MAX_Y, RAW_MIN_Y, 0, H);
  if (sx<0) sx=0; if (sx>=W) sx=W-1;
  if (sy<0) sy=0; if (sy>=H) sy=H-1;
  return true;
}

/* ============ selection groups ==================== */
void clearStepGroup() {
  btns[B_S100].selected = false;
  btns[B_S10].selected  = false;
  btns[B_S1].selected   = false;
  btns[B_S01].selected  = false;
}
void clearFeedGroup() {
  btns[B_F2000].selected = false;
  btns[B_F1000].selected = false;
  btns[B_F300].selected  = false;
  btns[B_F100].selected  = false;
}

/* ============ Layout builder ====================== */
static inline Button mkBtn(int x,int y,int w,int h,uint16_t c,const char* lbl,uint8_t id,bool momentary,bool selected=false){
  Button b;
  b.x=(int16_t)x; b.y=(int16_t)y; b.w=(int16_t)w; b.h=(int16_t)h;
  b.baseColor=c; b.label=lbl; b.id=id; b.momentary=momentary; b.selected=selected;
  return b;
}

void buildButtonsMain() {
  int16_t cellW = PAD_W/3;
  int16_t cellH = PAD_H/3;

  btns[B_YP]  = mkBtn(PAD_X+cellW,   PAD_Y,             cellW, cellH, COL_Y,    "Y+",  B_YP,  true);
  btns[B_YM]  = mkBtn(PAD_X+cellW,   PAD_Y+2*cellH,     cellW, cellH, COL_Y,    "Y-",  B_YM,  true);
  btns[B_XM]  = mkBtn(PAD_X,         PAD_Y+cellH,       cellW, cellH, COL_X,    "X-",  B_XM,  true);
  btns[B_XP]  = mkBtn(PAD_X+2*cellW, PAD_Y+cellH,       cellW, cellH, COL_X,    "X+",  B_XP,  true);
  btns[B_XY0] = mkBtn(PAD_X+cellW,   PAD_Y+cellH,       cellW, cellH, COL_WARN, "XY0", B_XY0, true);

  int16_t zGap = 8;
  int16_t zBtnH = (ZCOL_H - 2*zGap) / 3;
  btns[B_ZP] = mkBtn(ZCOL_X, ZCOL_Y,                      ZCOL_W, zBtnH, COL_Z,    "Z+", B_ZP, true);
  btns[B_Z0] = mkBtn(ZCOL_X, ZCOL_Y + zBtnH + zGap,       ZCOL_W, zBtnH, COL_WARN, "Z0", B_Z0, true);
  btns[B_ZM] = mkBtn(ZCOL_X, ZCOL_Y + 2*(zBtnH + zGap),   ZCOL_W, zBtnH, COL_Z,    "Z-", B_ZM, true);

  int16_t vGap = 8;
  int16_t sBtnH = (STEP_H - 3*vGap) / 4;
  btns[B_S100] = mkBtn(STEP_X, STEP_Y,                            STEP_W, sBtnH, COL_WARN, "100", B_S100, false);
  btns[B_S10]  = mkBtn(STEP_X, STEP_Y + (sBtnH+vGap),             STEP_W, sBtnH, COL_WARN, "10",  B_S10,  false);
  btns[B_S1]   = mkBtn(STEP_X, STEP_Y + 2*(sBtnH+vGap),           STEP_W, sBtnH, COL_WARN, "1",   B_S1,   false);
  btns[B_S01]  = mkBtn(STEP_X, STEP_Y + 3*(sBtnH+vGap),           STEP_W, sBtnH, COL_WARN, "0.1", B_S01,  false);

  int16_t fBtnH = (FEED_H - 3*vGap) / 4;
  btns[B_F2000] = mkBtn(FEED_X, FEED_Y,                           FEED_W, fBtnH, COL_WARN, "2000", B_F2000, false);
  btns[B_F1000] = mkBtn(FEED_X, FEED_Y + (fBtnH+vGap),            FEED_W, fBtnH, COL_WARN, "1000", B_F1000, false);
  btns[B_F300]  = mkBtn(FEED_X, FEED_Y + 2*(fBtnH+vGap),          FEED_W, fBtnH, COL_WARN, "300",  B_F300,  false);
  btns[B_F100]  = mkBtn(FEED_X, FEED_Y + 3*(fBtnH+vGap),          FEED_W, fBtnH, COL_WARN, "100",  B_F100,  false);

  int16_t homeW = 110, homeH = 36;
  btns[B_HOME] = mkBtn(W - homeW - 8, H - homeH - 4, homeW, homeH, COL_ACCENT, "$H", B_HOME, true);

  // defaults
  btns[selStep].selected = true;
  btns[selFeed].selected = true;
}

/* ===== ALT page line input state ===== */
static String inputLine;

// IMPORTANT: Make ENTER work immediately with single-char commands like '?'
inline void inputAppend(char c) { if (inputLine.length() < 200) inputLine += c; }
inline void inputBackspace()    { if (!inputLine.isEmpty())    inputLine.remove(inputLine.length() - 1); }
inline void inputSendAndClear() {
  // Send via big code (LF only)
  for (size_t i=0; i<inputLine.length(); ++i) 
  if (inputLine.length()) logAddLine(">> " + inputLine);
  inputLine = "";
}

/* Draw the input box (border + text) PAGE ALT */
void drawInputBox(bool clearBg = true) {
  if (clearBg) {
    tft.fillRect(INP_X, INP_Y, INP_W, INP_H, COL_BG);
  }
  tft.drawRect(INP_X, INP_Y, INP_W, INP_H, COL_MUTED);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(COL_TEXT, COL_BG);
  tft.drawString(inputLine, INP_X + 6, INP_Y + INP_H/2);
}

void buildButtonsAlt() {
  const int gap = 5;
  const int padR = 8;
  const int cols = 4;
  const int rows = 5;              // first 5 rows used for 20 keys

  const int KP_X = LOG_X + LOG_W + 5;   // 5 px to the right of logger
  const int KP_Y = LOG_Y;

  const int KP_W = W - KP_X - padR;
  const int KP_H = LOG_H+INP_H+POS_BOX_H+15;

  const int keyW = (KP_W - (cols - 1) * gap) / cols;
  const int keyH = (KP_H - (rows - 1) * gap) / rows;

  const char* labels20[] = {
    "7","8","9","?",     // r0
    "4","5","6","$",     // r1
    "1","2","3","G",     // r2
    "0",".","-","F",     // r3
    "|__|","X","Y","Z"   // r4 
  };

  auto colFor = [](const char* s)->uint16_t {
    char c = s[0];
    if ((c >= '0' && c <= '9') || c=='G' || c=='F' || c=='X' || c=='Y' || c=='Z') return COL_ACCENT;
    return COL_WARN;
  };

  uint8_t idx = 0;
  for (int r = 0; r < rows; ++r) {
    for (int c = 0; c < cols; ++c) {
      int x = KP_X + c * (keyW + gap);
      int y = KP_Y + r * (keyH + gap);
      const char* lab = labels20[idx];
      altBtns[idx] = mkBtn(x, y, keyW, keyH, colFor(lab), lab, (AltBtnId)idx, true);
      ++idx;
    }
  }

  const int extraY = KP_Y + rows * (keyH + gap);

  altBtns[A_ENTER] = mkBtn(KP_X,                                  extraY, keyW, keyH, COL_ACCENT, "En",    A_ENTER, true);
  altBtns[A_DEL]   = mkBtn(KP_X + (keyW + gap),                   extraY, keyW, keyH, COL_WARN,   "Del",   A_DEL,   true);
  altBtns[A_EQUAL] = mkBtn(KP_X + 2*(keyW + gap),                 extraY, keyW, keyH, COL_WARN,   "=",     A_EQUAL, true);

  // Back button bottom-right
  altBtns[A_BACK]  = mkBtn(W - 100 - 8, H - 36 - 4, 100, 36, COL_ACCENT, "Main", A_BACK, true);
}

void drawAllButtonsMain() { for (uint8_t i=0; i<BTN_COUNT; ++i) drawButton(btns[i]); }
void drawAllButtonsAlt()  { for (uint8_t i=0; i<ALT_BTN_COUNT; ++i) drawButton(altBtns[i]); }

/* ============== Page helpers ====================== */
inline bool hitPageToggle(int16_t sx, int16_t sy) {
  return (sx >= PAGE_BTN_X && sx < PAGE_BTN_X + PAGE_BTN_SIZE*2 &&
          sy >= PAGE_BTN_Y && sy < PAGE_BTN_Y + PAGE_BTN_SIZE);
}
void togglePage() {
  currentPage = (currentPage == PAGE_MAIN) ? PAGE_ALT : PAGE_MAIN;
  needsRedraw = true;
}
void buildButtonsForPage() {
  if (currentPage == PAGE_MAIN) buildButtonsMain();
  else                          buildButtonsAlt();
}

void logClearBox() {
  tft.fillRect(LOG_X, LOG_Y, LOG_W, LOG_H, COL_BG);
  tft.drawRect(LOG_X, LOG_Y, LOG_W, LOG_H, COL_FRAME);
}
void logRenderAll() {
  logClearBox();
  tft.setTextColor(COL_TEXT, COL_BG);
  tft.setTextFont(1);
  tft.setTextSize(1);
  const int16_t lineH = 16;
  for (uint8_t i = 0; i < logCount; ++i) {
    tft.setCursor(LOG_X + 6, LOG_Y + 6 + i*lineH);
    tft.print(logLines[i]);
  }
}
void logAddLine(const String& s) {
  if (logCount >= LOG_MAX_LINES) {
    logCount = 0;
    logClearBox();
  }
  logLines[logCount++] = s;
  logRenderAll();
}
// ===== Sprite versions of drawing helpers =====
static inline void drawStatusButtonS() {
  pageSpr.fillRoundRect(STATUS_BTN_X, STATUS_BTN_Y, STATUS_BTN_W, STATUS_BTN_H, 10, COL_PANEL);
  pageSpr.drawRoundRect(STATUS_BTN_X, STATUS_BTN_Y, STATUS_BTN_W, STATUS_BTN_H, 10, COL_FRAME);
  pageSpr.setTextDatum(MC_DATUM);
  pageSpr.setTextColor(COL_TEXT, COL_PANEL);
  pageSpr.setTextFont(1);
  pageSpr.setTextSize(2);
  pageSpr.drawCentreString("GRBL", STATUS_BTN_X + STATUS_BTN_W/2, STATUS_BTN_Y + STATUS_BTN_H/2, 1);
  pageSpr.setTextSize(1);
}

static inline void drawHeadersSameLineS() {
  pageSpr.setTextColor(COL_MUTED, COL_BG);
  pageSpr.setTextDatum(MC_DATUM);
  int16_t cx_xy   = PAD_X + PAD_W/2;
  int16_t cx_z    = ZCOL_X + ZCOL_W/2;
  int16_t cx_step = STEP_X + STEP_W/2;
  int16_t cx_feed = FEED_X + FEED_W/2;
  pageSpr.drawCentreString("X / Y",         cx_xy,   HEADER_Y, 1);
  pageSpr.drawCentreString("Z",             cx_z,    HEADER_Y, 1);
  pageSpr.drawCentreString("STEP (mm)",     cx_step, HEADER_Y, 1);
  pageSpr.drawCentreString("FEED (mm/min)", cx_feed, HEADER_Y, 1);
}

static inline void drawPanelFramesS() {
  pageSpr.fillScreen(COL_BG);

  auto drawPanelS = [&](int16_t x,int16_t y,int16_t w,int16_t h){
    pageSpr.fillRoundRect(x-4, y-4, w+8, h+8, 10, COL_PANEL);
    pageSpr.drawRoundRect(x-4, y-4, w+8, h+8, 10, COL_FRAME);
  };

  if (currentPage == PAGE_MAIN) {
    drawHeadersSameLineS();
    drawPanelS(PAD_X,  PAD_Y,  PAD_W,  PAD_H);
    drawPanelS(ZCOL_X, ZCOL_Y, ZCOL_W, ZCOL_H);
    drawPanelS(STEP_X, STEP_Y, STEP_W, STEP_H);
    drawPanelS(FEED_X, FEED_Y, FEED_W, FEED_H);
  } else {
    pageSpr.fillRoundRect(6, 4, W-12, H-BAR_H-6, 10, COL_PANEL);
    pageSpr.drawRoundRect(6, 4, W-12, H-BAR_H-6, 10, COL_FRAME);
  }

  // bottom bar
  pageSpr.fillRect(0, H-BAR_H, W, BAR_H, COL_PANEL);
  pageSpr.drawLine(0, H-BAR_H, W, H-BAR_H, COL_FRAME);

  // Page toggle (80x40)
  pageSpr.fillRoundRect(PAGE_BTN_X, PAGE_BTN_Y, PAGE_BTN_SIZE*2, PAGE_BTN_SIZE, 10, COL_ACCENT);
  pageSpr.drawRoundRect(PAGE_BTN_X, PAGE_BTN_Y, PAGE_BTN_SIZE*2, PAGE_BTN_SIZE, 10, COL_FRAME);
  pageSpr.setTextDatum(MC_DATUM);
  pageSpr.setTextColor(COL_TEXT, COL_ACCENT);
  pageSpr.drawCentreString((currentPage==PAGE_MAIN) ? "P2" : "P1",
                           PAGE_BTN_X + PAGE_BTN_SIZE, PAGE_BTN_Y + PAGE_BTN_SIZE/2, 1);

  drawStatusButtonS();
}

static inline void drawButtonS(const Button& b, bool pressed=false) {
  uint16_t face = b.baseColor;
  uint16_t border = COL_FRAME;
  if (!b.momentary && b.selected) { face = COL_ACCENT; border = COL_TEXT; }
  if (b.momentary && pressed) face = COL_ACCENT;

  pageSpr.fillRoundRect(b.x + (pressed?2:3), b.y + (pressed?3:4), b.w, b.h, BTN_R, COL_PRESSGLOW);
  pageSpr.fillRoundRect(b.x, b.y, b.w, b.h, BTN_R, face);
  pageSpr.drawRoundRect(b.x, b.y, b.w, b.h, BTN_R, border);

  pageSpr.setTextDatum(MC_DATUM);
  pageSpr.setTextColor(COL_TEXT, face);
  pageSpr.setTextFont(1);
  pageSpr.setTextSize(2);
  pageSpr.drawCentreString(b.label, b.x + b.w/2, b.y + b.h/2, 1);
  pageSpr.setTextSize(1);
}

static inline void drawAllButtonsMainS() { for (uint8_t i=0; i<BTN_COUNT; ++i) drawButtonS(btns[i]); }
static inline void drawAllButtonsAltS()  { for (uint8_t i=0; i<ALT_BTN_COUNT; ++i) drawButtonS(altBtns[i]); }

static inline void logClearBoxS() {
  pageSpr.fillRect(LOG_X, LOG_Y, LOG_W, LOG_H, COL_BG);
  pageSpr.drawRect(LOG_X, LOG_Y, LOG_W, LOG_H, COL_FRAME);
}
static inline void logRenderAllS() {
  logClearBoxS();
  pageSpr.setTextColor(COL_TEXT, COL_BG);
  pageSpr.setTextFont(1);
  pageSpr.setTextSize(1);
  const int16_t lineH = 16;
  for (uint8_t i = 0; i < logCount; ++i) {
    pageSpr.setCursor(LOG_X + 6, LOG_Y + 6 + i*lineH);
    pageSpr.print(logLines[i]);
  }
}
static inline void drawInputBoxS(bool clearBg = true) {
  if (clearBg) pageSpr.fillRect(INP_X, INP_Y, INP_W, INP_H, COL_BG);
  pageSpr.drawRect(INP_X, INP_Y, INP_W, INP_H, COL_MUTED);
  pageSpr.setTextDatum(ML_DATUM);
  pageSpr.setTextColor(COL_TEXT, COL_BG);
  pageSpr.drawString(inputLine, INP_X + 6, INP_Y + INP_H/2);
}

void drawPage() {
  if (useSprite) {
    // Draw whole page off-screen, then push once -> instant page switch
    pageSpr.fillSprite(COL_BG);
    drawPanelFramesS();

    if (currentPage == PAGE_MAIN) {
      drawAllButtonsMainS();
      pageSpr.setTextDatum(ML_DATUM);
      pageSpr.setTextColor(COL_MUTED, COL_PANEL);
    } else {
      drawAllButtonsAltS();
      logRenderAllS();
      drawInputBoxS();
      pageSpr.setTextDatum(ML_DATUM);
      pageSpr.setTextColor(COL_MUTED, COL_PANEL);
    }

    tft.startWrite();
    pageSpr.pushSprite(0, 0);
    tft.endWrite();

  } else {
    // Fallback: your original immediate-mode path (kept as-is)
    drawPanelFrames();
    if (currentPage == PAGE_MAIN) {
      drawAllButtonsMain();
      tft.setTextDatum(ML_DATUM);
      tft.setTextColor(COL_MUTED, COL_PANEL);
    } else {
      drawAllButtonsAlt();
      logRenderAll();
      drawInputBox();
      tft.setTextDatum(ML_DATUM);
      tft.setTextColor(COL_MUTED, COL_PANEL);
    }
  }
}

/* ================= Setup / Loop =================== */
void setup() {
  Serial.begin(115200); // optional

  tft.init();
  tft.setRotation(ROTATION);
  W = tft.width();
  H = tft.height();
    pageSpr.setColorDepth(16);                 // 16-bit color
    if (pageSpr.createSprite(W, H) == nullptr) {
      useSprite = false;                       // Not enough RAM? fallback to old path
    }

  tft.setSwapBytes(true);  // sprite pixel order is little-endian; this speeds the transfer
  tft.initDMA();           // enable DMA path inside TFT_eSPI
  

  PAGE_BTN_X =  6; // left margin
  PAGE_BTN_Y = 278;

  // status button just to the right of page toggle
  STATUS_BTN_X = PAGE_BTN_X + PAGE_BTN_SIZE*2 + 8;
  STATUS_BTN_Y = PAGE_BTN_Y;

  tft.setTextFont(1);
  tft.setTextSize(1);
  tft.setTextColor(COL_TEXT, COL_BG);
  tft.setTextDatum(TL_DATUM);

  SPI.begin();
  ts.begin(SPI);
  ts.setRotation(ROTATION);

  buildButtonsForPage();
  drawPage();
  needsRedraw = false;

  Serial.println("UI ready.");
}

void loop() {


  // --- Page redraw if needed ---
  if (needsRedraw) {
    buildButtonsForPage();
    drawPage();
    needsRedraw = false;
  }

  // --- Touch handling (stable press, no repeat, no blink) ---
  int16_t sx, sy;
  bool touchedNow = readTouch(sx, sy);

  uint32_t now = millis();
  if (touchedNow) {
    if (!touchDown && (now - lastTouchMs) >= TOUCH_DEBOUNCE_MS) {
      // TOUCH DOWN
      lastTouchMs = now;
      touchDown   = true;
      actionSent  = false;

      // Page toggle
      if (hitPageToggle(sx, sy)) { togglePage(); return; }

      if (currentPage == PAGE_MAIN) {
        activeBtnId = hitTestMain(sx, sy);
        if (activeBtnId >= 0) {
          Button &b = btns[activeBtnId];
          drawButton(b, true);

          if (b.momentary && !actionSent) {
            switch (activeBtnId) {
              case B_XP:  gcodeJog('X', +stepValueFor(selStep), feedValueFor(selFeed)); break;
              case B_XM:  gcodeJog('X', -stepValueFor(selStep), feedValueFor(selFeed)); break;
              case B_YP:  gcodeJog('Y', +stepValueFor(selStep), feedValueFor(selFeed)); break;
              case B_YM:  gcodeJog('Y', -stepValueFor(selStep), feedValueFor(selFeed)); break;
              case B_XY0: gcodeGoXYZero(feedValueFor(selFeed)); break;

              case B_ZP:  gcodeJog('Z', +stepValueFor(selStep), feedValueFor(selFeed)); break;
              case B_ZM:  gcodeJog('Z', -stepValueFor(selStep), feedValueFor(selFeed)); break;
              case B_Z0:  gcodeGoAxisZero('Z', feedValueFor(selFeed)); break;

              case B_HOME: gcodeHome(); break;
            }
            actionSent = true;
          }
        }
      } else { // PAGE_ALT
        activeAltId = hitTestAlt(sx, sy);
        if (activeAltId >= 0) {
          Button &b = altBtns[activeAltId];
          drawButton(b, true);

          switch (activeAltId) {
            // digits
            case A_K0: inputAppend('0'); break;
            case A_K1: inputAppend('1'); break;
            case A_K2: inputAppend('2'); break;
            case A_K3: inputAppend('3'); break;
            case A_K4: inputAppend('4'); break;
            case A_K5: inputAppend('5'); break;
            case A_K6: inputAppend('6'); break;
            case A_K7: inputAppend('7'); break;
            case A_K8: inputAppend('8'); break;
            case A_K9: inputAppend('9'); break;

            // specials & words
            case A_KQMARK:  inputAppend('?'); break;
            case A_KDOLLAR: inputAppend('$'); break;
            case A_KG:      inputAppend('G'); break;
            case A_KF:      inputAppend('F'); break;
            case A_KDOT:    inputAppend('.'); break;
            case A_KMINUS:  inputAppend('-'); break;
            case A_KSPACE:  inputAppend(' '); break;
            case A_X:       inputAppend('X'); break;
            case A_Y:       inputAppend('Y'); break;
            case A_Z:       inputAppend('Z'); break;
            case A_EQUAL:   inputAppend('='); break;

            // editing / actions
            case A_DEL:     inputBackspace(); break;
            case A_ENTER:   inputSendAndClear(); break;
            case A_BACK:    togglePage(); break;
          }

          if (currentPage == PAGE_ALT) {
            drawInputBox();
          }
          actionSent = true;
        }
      }
    }
    // HOLD: keep pressed look; do nothing

  } else {
    if (touchDown && (now - lastTouchMs) >= TOUCH_DEBOUNCE_MS) {
      // TOUCH UP
      lastTouchMs = now;

      if (currentPage == PAGE_MAIN) {
        if (activeBtnId >= 0) {
          Button &b = btns[activeBtnId];

          // Handle selection groups on release
          switch (activeBtnId) {
            case B_S100: case B_S10: case B_S1: case B_S01:
              clearStepGroup(); selStep = activeBtnId; btns[selStep].selected = true; refreshSelections(); break;

            case B_F2000: case B_F1000: case B_F300: case B_F100:
              clearFeedGroup(); selFeed = activeBtnId; btns[selFeed].selected = true; refreshSelections(); break;

            default: break;
          }
          drawButton(b, false);
        }
        activeBtnId = -1;
      } else {
        if (activeAltId >= 0) {
          Button &b = altBtns[activeAltId];
          if (currentPage == PAGE_ALT) drawButton(b, false);
        }
        activeAltId = -1;
      }

      touchDown  = false;
      actionSent = false;
    }
  }
}