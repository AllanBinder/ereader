#include "ui.h"

#include <GxEPD2_BW.h>
#include <SPI.h>
#include <U8g2_for_Adafruit_GFX.h>
#include <esp_task_wdt.h>
#include <time.h>

#include <algorithm>
#include <climits>
#include <vector>

#include "config.h"
#include "panel.h"
#include "pins.h"

namespace {

// ------------------------------------------------------------ menu layout
//
// Grid of cards. Header band (solid black, sensor line in white), then
// menuRows() rows of menuCols() tiles with a gutter between them, then the
// usual status strip. Each tile carries its 1-based index as a large numeral
// top right, a short accent hairline, a bold label and a small state line
// bottom left. The tile at the cursor is filled black with the same content
// in white. Rows scroll to keep the cursor's row visible when there are more
// tiles than fit.
//
// Columns: as many as the width holds at MENU_MIN_TILE_W, panel 1's tile,
// the narrowest a two line label has been laid out for. Panel 0:
// (648 + 12) / (218 + 12) = 2 columns of (648 - 12) / 2 = 318 px (ui::TILE_W);
// panel 1: (448 + 12) / 230 = 2 of 218. Panel 2, the 792x272 bar: 772 / 230 =
// 3 columns of (760 - 2*12) / 3 = 245 px.
//
// Rows: the grid runs from menuGridTop() to ui::GRID_BOTTOM, 16 px above the
// status rule. The row count is as many rows as fit at MENU_MIN_TILE_H (the
// deepest tile content below needs 124 px), and the tiles then share the
// height evenly. Panel 0: (880 - 64 + 12) / (126 + 12) = 6 rows of
// (880 - 64 - 5*12) / 6 = 126 px. Panel 1: (760 - 64 + 12) / 138 = 5 rows of
// (760 - 64 - 4*12) / 5 = 129 px. The band, gutter, tile width and pad are
// the shared card constants in ui.h, which the numbers view draws with as
// well.
//
// A short canvas (ui::SHORT_CANVAS: fewer than two rows of full tiles fit)
// takes the thin 28 px band and the compact tile instead: no big numeral and
// no accent, label and state line only, with the index in 7x14 bold top
// right, deepest content at 81 px. Panel 2: the grid runs from 28 + 12 = 40
// to 232, (232 - 40 + 12) / (82 + 12) = 2 rows of (232 - 40 - 12) / 2 = 90 px,
// so 6 tiles on screen.
static const int MENU_MIN_TILE_W = 218;
static const int MENU_MIN_TILE_H = ui::CARD_MIN_H;
static const int MENU_COMPACT_MIN_TILE_H = 82;
static const int MENU_GUTTER = ui::GRID_GUTTER;

int menuCols() {
  int cols = (SCREEN_W - 2 * TEXT_MARGIN_X + MENU_GUTTER) /
             (MENU_MIN_TILE_W + MENU_GUTTER);
  return cols < 1 ? 1 : cols;
}

int menuTileW() {
  const int cols = menuCols();
  return (SCREEN_W - 2 * TEXT_MARGIN_X - MENU_GUTTER * (cols - 1)) / cols;
}

// 44 + 20 = 64 under the full band, 28 + 12 = 40 under the thin one.
int menuGridTop() {
  return ui::SHORT_CANVAS ? ui::HEADER_SHORT_H + MENU_GUTTER
                          : ui::HEADER_H + ui::GRID_TOP_GAP;
}

int menuRows() {
  const int minH =
      ui::SHORT_CANVAS ? MENU_COMPACT_MIN_TILE_H : MENU_MIN_TILE_H;
  int rows = (ui::GRID_BOTTOM - menuGridTop() + MENU_GUTTER) /
             (minH + MENU_GUTTER);
  return rows < 1 ? 1 : rows;
}

int menuTileH() {
  const int rows = menuRows();
  return (ui::GRID_BOTTOM - menuGridTop() - MENU_GUTTER * (rows - 1)) / rows;
}

static const int MENU_TILE_PAD = ui::TILE_PAD;
// Offsets inside a tile, from its top edge. The numeral is fub42 (42 px
// digits), the label helvB24 (24 px caps), the sub line 7x14. A label wider
// than the tile wraps onto a second line at the tighter LABEL2 pitch, which
// pushes the sub line down to SUB2 (7x14 descenders end at 124, inside the
// 126 px tile).
static const int MENU_NUM_BASELINE = 50;
static const int MENU_ACCENT_Y = 60;
static const int MENU_ACCENT_W = ui::TILE_ACCENT_W;
static const int MENU_LABEL_BASELINE = 92;
static const int MENU_SUB_BASELINE = 114;
static const int MENU_LABEL2_BASELINE_1 = 82;
static const int MENU_LABEL2_BASELINE_2 = 106;
static const int MENU_SUB2_BASELINE = 121;
// Compact tile, same fonts less the numeral: one line label 40 (caps from
// 15), sub 64; two line label 32 and 56, sub 78 (7x14 descenders end at 81).
// The 7x14 bold index sits top right on baseline 20, and the label wraps
// short of it.
static const int MENU_C_NUM_BASELINE = 20;
static const int MENU_C_LABEL_BASELINE = 40;
static const int MENU_C_SUB_BASELINE = 64;
static const int MENU_C_LABEL2_BASELINE_1 = 32;
static const int MENU_C_LABEL2_BASELINE_2 = 56;
static const int MENU_C_SUB2_BASELINE = 78;

// The panel. Which one is decided at boot from NVS (src/panel.h); the driver
// and its frame buffer are created on the heap by the panel table's factory,
// so only the chosen panel's buffer exists. Everything here draws through the
// thin panel::Display interface and never names a GxEPD2 driver type.
//
// The page buffer is the full native panel height, so a whole frame is
// composed in one firstPage/nextPage pass (nativeW * nativeH / 8 bytes, the
// biggest being panel 0's 78200). Rotation is a coordinate transform inside
// GxEPD2, so it costs no extra RAM.
//
// Panel 0's driver is GxEPD2_576_T81_Fast from src/drivers/, NOT the stock
// GxEPD2_576_GDEH0576T81. The stock driver has hasFastPartialUpdate = false
// and runs the full waveform even on a partial window, so every page turn
// flashes for seconds. Ours carries a real partial LUT and a previous-frame
// shadow buffer. Its writeImage() always consumes a whole native frame and
// ignores the window rectangle, so every partial window here is the whole
// screen (see beginFrame); the stock drivers are happy with that too.
panel::Display *disp = nullptr;

// Shorthand for the drawing canvas.
Adafruit_GFX &canvas() { return disp->gfx(); }

// Bumped whenever the frame buffer stops holding what the last kept frame
// left in it: every firstPage (GxEPD2_BW clears the whole buffer there), a
// rotation change (same bits, other coordinates) and a new driver. Speed read
// compares it across its frames to know its last compose is still in the
// buffer (ui::frameEpoch).
uint32_t bufferEpoch = 0;

void firstPageClear() {
  bufferEpoch++;
  disp->firstPage();
}

U8G2_FOR_ADAFRUIT_GFX u8g2;

uint16_t partialsSinceFull = 0;

// Set while the frame on glass is an image page. The next text page has to
// clear it with a full refresh, otherwise the artwork ghosts through.
bool lastFrameWasImage = false;

const uint8_t *FONT_BODY = u8g2_font_profont22_mf;
const uint8_t *FONT_BODY_LARGE = u8g2_font_profont29_mf;
const uint8_t *FONT_SMALL = u8g2_font_6x12_tf;
// Page counter on the right of the status strip, a step larger than the title.
const uint8_t *FONT_STATUS_RIGHT = u8g2_font_7x14_tf;
// Same metrics, bold: the page counter is the one thing in the strip a reader
// looks for on purpose, so it carries the weight.
const uint8_t *FONT_STATUS_RIGHT_BOLD = u8g2_font_7x14B_tf;
const uint8_t *FONT_BIG = u8g2_font_fub42_tn;
// Menu tile label: bold proportional, the one non-monospace face in the build
// (about 3 KB of flash). The tile numeral reuses FONT_BIG, already linked for
// the jump screen.
const uint8_t *FONT_MENU_LABEL = u8g2_font_helvB24_tr;
const uint8_t *FONT_MENU_NUM = u8g2_font_fub42_tn;
// Speed read status line, the font the rsvp_eink proof of concept used.
const uint8_t *FONT_CAPTION = u8g2_font_helvB14_tr;
// Speed read HUD: small tracked caps and condensed readout numerals.
const uint8_t *FONT_HUD = u8g2_font_helvB10_tr;
const uint8_t *FONT_HUD_NUM = u8g2_font_logisoso38_tr;

// Portrait rotation to go back to when speed read leaves landscape. Set by
// beginPanel from the panel's DISPLAY_ROTATION. Unused on a landscape UI.
uint8_t portraitRotation = 1;
bool landscape = false;

// Speed read drive state and the bench console's hook overrides (src/bench.cpp).
// The overrides are stored even outside speed read and applied by
// setSpeedDrive(true) on top of the speed profile; each ov* flag says the
// console set that one, and it then wins over the profile until
// benchClearOverrides(). A panel without a profile behaves as before the
// profiles existed: every hook off unless the console set it, and
// INT_MIN = no forced temperature override, so entry forces RSVP_LUT_TEMP.
bool speedDriveOn = false;
speed::Mode speedModeSet = speed::Mode::NORMAL;
bool benchKeepPower = false, ovKeepPower = false;
bool benchSingleWrite = false, ovSingleWrite = false;
// Bench waveform (src/bench.cpp lut / volt), applied only while speed read
// drives the panel, like keep power and single write. ovLut with
// benchLutFromSpec: built here from benchLutSpec (benchSetLut); without it:
// the caller's raw table benchLut (benchSetCustomLut, nullptr = OTP).
bool ovLut = false;
bool benchLutFromSpec = false;
struct LutSpec {
  speed::LutKind kind;
  int frames, frameRate;
  bool pol;
};
LutSpec benchLutSpec = {speed::LutKind::OTP, 0, 0, false};
const uint8_t *benchLut = nullptr;
size_t benchLutLen = 0;
uint8_t benchVsh1 = 0, benchVsh2 = 0, benchVsl = 0;
bool benchSkipPrevious = false, ovSkipPrevious = false;
bool benchLean = false, ovLean = false;  // lean timing, speed read only like the hooks above
uint32_t benchSpiHz = 0;  // 0 = the panel's speedSpiHz
int benchTemp = INT_MIN;
// Panel temperature sampled at the last drive on (speed::panelTempC), which
// the profile's LUT frames follow; INT16_MIN = unknown or never sampled.
int driveTempC = INT16_MIN;

// One text grid. The numbers come from config.h (cols and rows computed for
// the active panel by panel::applyGeometry) and must match the host
// converter, which paginated the page we are about to draw.
struct TextGrid {
  const uint8_t *font;
  int cols;
  int rows;
  int firstBaselineY;
  int pitch;
};

TextGrid gridFor(uint8_t fontId) {
  if (fontId == BOOK_FONT_PROFONT29) {
    return TextGrid{FONT_BODY_LARGE, TEXT_LARGE_COLS, TEXT_LARGE_ROWS,
                    TEXT_LARGE_FIRST_BASELINE_Y, TEXT_LARGE_LINE_PITCH};
  }
  return TextGrid{FONT_BODY, TEXT_COLS, TEXT_ROWS, TEXT_FIRST_BASELINE_Y,
                  TEXT_LINE_PITCH};
}

// Full refreshes hold the panel busy for seconds and a flaky BUSY wire can
// stretch that to GxEPD2's 10 s timeout; feed the watchdog from the busy poll
// so a slow refresh can never starve it into a reboot loop.
void feedWatchdogWhileBusy(const void *) { esp_task_wdt_reset(); }

void setInk(bool inverted) {
  u8g2.setForegroundColor(inverted ? GxEPD_WHITE : GxEPD_BLACK);
  u8g2.setBackgroundColor(inverted ? GxEPD_BLACK : GxEPD_WHITE);
}

// Chooses full vs fast partial refresh and rations full refreshes so partial
// ghosting gets cleared periodically.
//
// setFastRefresh() is what actually picks the waveform. It swaps the LUT the
// driver loads at the next _InitDisplay(): the partial LUT drives only the
// pixels that changed against the shadow buffer (no white flash, a few hundred
// ms), the full LUT drives every pixel through the ghost-clearing waveform. It
// has to be set before setFullWindow/setPartialWindow so the flag is right by
// the time nextPage() reaches the driver.
//
// Shadow coherence is the driver's job and needs nothing here: a full frame
// goes through writeImageForFullRefresh(), which rewrites the shadow from the
// same bitmap it just sent, so the next partial interleaves against exactly
// what is on glass.
void beginFrame(bool forceFull) {
  bool full = forceFull || partialsSinceFull >= PARTIALS_BEFORE_FULL;
  if (full) {
    disp->setFastRefresh(false);
    disp->setFullWindow();
    partialsSinceFull = 0;
  } else {
    disp->setFastRefresh(true);
    // Whole screen on purpose: the panel 0 driver ignores the window
    // rectangle and always ships a full frame, so a smaller window would send
    // stale rows.
    disp->setPartialWindow(0, 0, disp->width(), disp->height());
    partialsSinceFull++;
  }
}

// Local HH:MM, or an empty string while the clock has never been set. The
// reader has no RTC battery: time(nullptr) is 1970 from a cold boot until a
// news sync runs SNTP, and stays right from then until the board loses power.
// Nothing here ticks on its own, which is the point: the strip is redrawn only
// when a frame is drawn, so the clock advances on a page turn and never wakes
// the panel by itself.
String clockText() {
  time_t now = time(nullptr);
  if (now < (time_t)NET_TIME_SANE_EPOCH) return String();
  now += (time_t)CLOCK_UTC_OFFSET_MINUTES * 60;
  struct tm parts;
  gmtime_r(&now, &parts);  // already shifted, so gmtime and not localtime
  char buf[8];
  snprintf(buf, sizeof(buf), "%02d:%02d", parts.tm_hour, parts.tm_min);
  return String(buf);
}

// Cuts `s` so it fits `maxW` pixels in the current u8g2 font, ending in ".."
// when anything was dropped. Two dots rather than an ellipsis glyph: the
// bitmap fonts in this build do not all carry one.
String fitText(const String &s, int maxW) {
  if ((int)u8g2.getUTF8Width(s.c_str()) <= maxW) return s;
  String out = s;
  while (out.length() > 1) {
    out.remove(out.length() - 1);
    String probe = out;
    probe.trim();
    probe += "..";
    if ((int)u8g2.getUTF8Width(probe.c_str()) <= maxW) return probe;
  }
  return String("..");
}

// Greedy word wrap of `s` into at most `maxLines` lines that fit `maxW` in
// the current font. Every line goes through fitText, so a single word wider
// than a line is cut with "..", and so is the last line when the text does
// not fit in `maxLines` (the remainder is packed onto it and cut).
std::vector<String> wrapText(const String &s, int maxW, int maxLines) {
  std::vector<String> words;
  int start = 0;
  while (start <= (int)s.length()) {
    int sp = s.indexOf(' ', start);
    if (sp < 0) sp = s.length();
    if (sp > start) words.push_back(s.substring(start, sp));
    start = sp + 1;
  }
  std::vector<String> lines;
  size_t i = 0;
  while (i < words.size()) {
    if ((int)lines.size() == maxLines - 1) {
      String rest = words[i];
      for (size_t j = i + 1; j < words.size(); j++) rest += " " + words[j];
      lines.push_back(fitText(rest, maxW));
      return lines;
    }
    String line = words[i++];
    while (i < words.size()) {
      String probe = line + " " + words[i];
      if ((int)u8g2.getUTF8Width(probe.c_str()) > maxW) break;
      line = probe;
      i++;
    }
    lines.push_back(fitText(line, maxW));
  }
  if (lines.empty()) lines.push_back(String());
  return lines;
}

// One menu card at (x, y). `selected` inverts it.
void drawMenuTile(int x, int y, int index, const ui::MenuTile &tile,
                  bool selected) {
  const int w = menuTileW();
  const int h = menuTileH();
  if (selected) {
    canvas().fillRect(x, y, w, h, GxEPD_BLACK);
  } else {
    canvas().drawRect(x, y, w, h, GxEPD_BLACK);
  }
  const uint16_t ink = selected ? GxEPD_WHITE : GxEPD_BLACK;
  setInk(selected);

  char num[8];
  snprintf(num, sizeof(num), "%02d", index);

  if (ui::SHORT_CANVAS) {
    // Compact card: small index top right, the label wraps short of it.
    u8g2.setFont(FONT_STATUS_RIGHT_BOLD);
    const int nw = u8g2.getUTF8Width(num);
    u8g2.setCursor(x + w - MENU_TILE_PAD - nw, y + MENU_C_NUM_BASELINE);
    u8g2.print(num);
    const int textW = w - 2 * MENU_TILE_PAD;
    u8g2.setFont(FONT_MENU_LABEL);
    std::vector<String> lines = wrapText(tile.label, textW - nw - 8, 2);
    int subBaseline = MENU_C_SUB_BASELINE;
    if (lines.size() == 1) {
      u8g2.setCursor(x + MENU_TILE_PAD, y + MENU_C_LABEL_BASELINE);
      u8g2.print(lines[0]);
    } else {
      u8g2.setCursor(x + MENU_TILE_PAD, y + MENU_C_LABEL2_BASELINE_1);
      u8g2.print(lines[0]);
      u8g2.setCursor(x + MENU_TILE_PAD, y + MENU_C_LABEL2_BASELINE_2);
      u8g2.print(lines[1]);
      subBaseline = MENU_C_SUB2_BASELINE;
    }
    if (tile.sub.length()) {
      u8g2.setFont(FONT_STATUS_RIGHT);
      u8g2.setCursor(x + MENU_TILE_PAD, y + subBaseline);
      u8g2.print(fitText(tile.sub, textW));
    }
    return;
  }

  // Index numeral, top right.
  u8g2.setFont(FONT_MENU_NUM);
  int nw = u8g2.getUTF8Width(num);
  u8g2.setCursor(x + w - MENU_TILE_PAD - nw, y + MENU_NUM_BASELINE);
  u8g2.print(num);

  // Accent hairline, 2 px, so the card reads as a card and not a box.
  canvas().fillRect(x + MENU_TILE_PAD, y + MENU_ACCENT_Y, MENU_ACCENT_W, 2,
                    ink);

  // Label and state line, bottom left. A label that does not fit on one line
  // ("News brief  2026-09-12 08:07") wraps onto two, and the state line moves
  // down to make room.
  const int textW = w - 2 * MENU_TILE_PAD;
  u8g2.setFont(FONT_MENU_LABEL);
  std::vector<String> lines = wrapText(tile.label, textW, 2);
  int subBaseline = MENU_SUB_BASELINE;
  if (lines.size() == 1) {
    u8g2.setCursor(x + MENU_TILE_PAD, y + MENU_LABEL_BASELINE);
    u8g2.print(lines[0]);
  } else {
    u8g2.setCursor(x + MENU_TILE_PAD, y + MENU_LABEL2_BASELINE_1);
    u8g2.print(lines[0]);
    u8g2.setCursor(x + MENU_TILE_PAD, y + MENU_LABEL2_BASELINE_2);
    u8g2.print(lines[1]);
    subBaseline = MENU_SUB2_BASELINE;
  }
  if (tile.sub.length()) {
    u8g2.setFont(FONT_STATUS_RIGHT);
    u8g2.setCursor(x + MENU_TILE_PAD, y + subBaseline);
    u8g2.print(fitText(tile.sub, textW));
  }
}

void drawStatusStrip(const String &left, const String &right,
                     bool boldRight = false) {
  canvas().drawFastHLine(TEXT_MARGIN_X, STATUS_RULE_Y,
                         disp->width() - 2 * TEXT_MARGIN_X, GxEPD_BLACK);
  u8g2.setFont(FONT_SMALL);
  setInk(false);
  u8g2.setCursor(TEXT_MARGIN_X, STATUS_BASELINE_Y);
  u8g2.print(left);
  int leftEnd = TEXT_MARGIN_X + u8g2.getUTF8Width(left.c_str());

  u8g2.setFont(boldRight ? FONT_STATUS_RIGHT_BOLD : FONT_STATUS_RIGHT);
  int w = u8g2.getUTF8Width(right.c_str());
  int rightStart = disp->width() - TEXT_MARGIN_X - w;
  u8g2.setCursor(rightStart, STATUS_BASELINE_Y);
  u8g2.print(right);

  // Clock in the middle of the strip, dropped rather than overlapped when the
  // title and the right hand text leave it no room (the menu's help line is
  // wide enough to claim the whole strip).
  String clock = clockText();
  if (clock.length()) {
    u8g2.setFont(FONT_STATUS_RIGHT);
    int wc = u8g2.getUTF8Width(clock.c_str());
    int x = (disp->width() - wc) / 2;
    if (x > leftEnd + 12 && x + wc < rightStart - 12) {
      u8g2.setCursor(x, STATUS_BASELINE_Y);
      u8g2.print(clock);
    }
  }
}

}  // namespace

namespace ui {

// ----- primitives for views composed outside this file (numbers_view) -----

void setFont(Font f) {
  switch (f) {
    case Font::SMALL: u8g2.setFont(FONT_SMALL); break;
    case Font::STATUS: u8g2.setFont(FONT_STATUS_RIGHT); break;
    case Font::STATUS_BOLD: u8g2.setFont(FONT_STATUS_RIGHT_BOLD); break;
    case Font::BODY: u8g2.setFont(FONT_BODY); break;
    case Font::LABEL: u8g2.setFont(FONT_MENU_LABEL); break;
    case Font::NUM: u8g2.setFont(FONT_MENU_NUM); break;
    case Font::CAPTION: u8g2.setFont(FONT_CAPTION); break;
    case Font::HUD: u8g2.setFont(FONT_HUD); break;
    case Font::HUD_NUM: u8g2.setFont(FONT_HUD_NUM); break;
  }
}

int textWidth(const String &s) { return (int)u8g2.getUTF8Width(s.c_str()); }

void setInk(bool inverted) { ::setInk(inverted); }

void printAt(int x, int baselineY, const String &s) {
  u8g2.setCursor(x, baselineY);
  u8g2.print(s);
}

String fitText(const String &s, int maxW) { return ::fitText(s, maxW); }

void drawStatusStrip(const String &left, const String &right, bool boldRight) {
  ::drawStatusStrip(left, right, boldRight);
}

Adafruit_GFX &gfx() { return canvas(); }

// 180 degrees is two quarter turns: 1 <-> 3 on a portrait UI, 0 <-> 2 on a
// landscape one.
uint8_t flippedRotation(uint8_t rotation) { return (rotation + 2) & 3; }

bool validRotation(uint8_t rotation) {
  return rotation == DISPLAY_ROTATION ||
         rotation == flippedRotation(DISPLAY_ROTATION);
}

int headerH() { return SHORT_CANVAS ? HEADER_SHORT_H : HEADER_H; }

int headerBaselineY() {
  return SHORT_CANVAS ? HEADER_SHORT_BASELINE_Y : HEADER_BASELINE_Y;
}

std::vector<String> wrapText(const String &s, int maxW, int maxLines) {
  return ::wrapText(s, maxW, maxLines);
}

void setRotation(uint8_t rotation) {
  if (!validRotation(rotation)) rotation = DISPLAY_ROTATION;
  bufferEpoch++;
  disp->setRotation(rotation);
}

uint8_t rotation() { return disp->getRotation(); }

void setLandscape(bool on) {
  if (on == landscape) return;
  // A landscape UI (even DISPLAY_ROTATION) already runs on the native frame:
  // nothing to turn either way, only the state is kept so the pair balances.
  if ((DISPLAY_ROTATION & 1) == 0) {
    landscape = on;
    return;
  }
  bufferEpoch++;
  if (on) {
    // Every panel in the table has a landscape native frame, so the even
    // rotation one step back from the portrait one is the same way up.
    portraitRotation = disp->getRotation();
    disp->setRotation(portraitRotation == 3 ? 2 : 0);
  } else {
    disp->setRotation(portraitRotation);
  }
  landscape = on;
}

namespace {

// Effective settings: the console's override, else the profile's, else
// (no profile) the console's stored value, false by default.
bool effKeepPower(const speed::Profile *p) {
  return ovKeepPower || !p ? benchKeepPower : p->keepPower;
}
bool effSingleWrite(const speed::Profile *p) {
  return ovSingleWrite || !p ? benchSingleWrite : p->singleWrite;
}
bool effLean(const speed::Profile *p) { return ovLean || !p ? benchLean : p->lean; }
int effForcedTemp(const speed::Profile *p) {
  if (benchTemp != INT_MIN) return benchTemp;
  return p ? p->forcedTemp : RSVP_LUT_TEMP;
}

// The partial LUT and skip previous speed read runs with right now: the
// console's LUT override, else the profile's LUT built for driveTempC
// (sampled at drive on), else OTP. A profile's skipPrevious only counts while
// the LUT in force is a known HOLD table: NOHOLD and BALANCED read the
// previous frame, so skipping its write would leave old words on glass. A
// console skip previous override is taken as given. Pure: nothing is built
// or sent.
struct LutPlan {
  bool raw;      // the console's benchSetCustomLut table (kind unknown)
  LutSpec spec;  // kind OTP = no custom LUT; frames clamped 1..63 otherwise
  bool skip;
};
LutPlan planLut() {
  const speed::Profile *p = activeSpeedProfile();
  LutPlan l = {false, {speed::LutKind::OTP, 0, 0, false}, false};
  if (ovLut && !benchLutFromSpec) {
    l.raw = benchLut != nullptr;
  } else if (ovLut) {
    l.spec = benchLutSpec;
  } else if (p && p->lut != speed::LutKind::OTP) {
    l.spec = {p->lut, speed::framesForTemp(p->frames, driveTempC), p->frameRate, false};
  }
  if (l.spec.kind == speed::LutKind::OTP) {
    l.spec.frames = 0;
    l.spec.frameRate = 0;
  } else if (l.spec.frames < 1) {
    l.spec.frames = 1;
  } else if (l.spec.frames > 63) {
    l.spec.frames = 63;
  }
  if (ovSkipPrevious || !p) l.skip = benchSkipPrevious;
  else l.skip = p->skipPrevious && !l.raw && l.spec.kind == speed::LutKind::HOLD;
  return l;
}

// Puts the planned LUT and skip previous on the panel (drive on), or the OTP
// waveform and no skip (off). speed::buildLut rebuilds one static table in
// place; the driver keeps the pointer and reloads it on setCustomLut.
void applyLutAndSkip(bool on) {
  const uint8_t *lut = nullptr;
  size_t len = 0;
  bool skip = false;
  if (on) {
    const LutPlan l = planLut();
    if (l.raw) {
      lut = benchLut;
      len = benchLutLen;
    } else {
      lut = speed::buildLut(l.spec.kind, l.spec.frames, l.spec.frameRate, l.spec.pol, len);
    }
    skip = l.skip;
  }
  disp->setCustomLut(lut, len);
  disp->setSkipPrevious(skip);
}

// Profile (or fixed) drive minus the SPI clock and the cut, which only
// setSpeedDrive itself sets (rsvp moves the cut per flash on panel 0, so a
// re-apply must not reset it).
void applyDriveHooks(bool on) {
  const speed::Profile *p = on ? activeSpeedProfile() : nullptr;
  disp->setForcedTemp(on ? effForcedTemp(p) : -1);
  // Keep power and single write never outlive speed read: the rails must not
  // stay up while the reader idles on a page.
  disp->setKeepPower(on && effKeepPower(p));
  disp->setSingleWrite(on && effSingleWrite(p));
  // The waveform and source levels are speed read only too: reading pages
  // and menus always run the OTP waveform.
  applyLutAndSkip(on);
  disp->setSourceLevels(on ? benchVsh1 : 0, on ? benchVsh2 : 0, on ? benchVsl : 0);
  disp->setLeanTiming(on && effLean(p));
}

// Samples the panel temperature for the profile's LUT, once per drive on.
// Only on a panel with a profile, so panel 0 never pays for the read.
void sampleDriveTemp() {
  driveTempC = activeSpeedProfile() ? speed::panelTempC() : INT16_MIN;
}

void logDrive() {
  const speed::Profile *p = activeSpeedProfile();
  if (!p) return;
  const LutPlan l = planLut();
  char t[12];
  if (driveTempC == INT16_MIN) snprintf(t, sizeof(t), "?");
  else snprintf(t, sizeof(t), "%d C", driveTempC);
  Serial.printf("speed drive: %s, LUT %s%s F %d, panel %s%s\n", p->name,
                l.raw ? "raw" : speed::lutKindName(l.spec.kind),
                ovLut ? " (console)" : "", l.spec.frames, t,
                (ovKeepPower || ovSingleWrite || ovLean || ovSkipPrevious ||
                 benchTemp != INT_MIN)
                    ? ", console overrides"
                    : "");
}

}  // namespace

// Speed read's drive is tuned for one panel's waveform; on a panel without
// speedReadSupported the menu hides speed read, and these leave the panel at
// its normal clock and LUT if anything calls them anyway.
void setSpeedDrive(bool on) {
  const panel::Spec &s = panel::activeSpec();
  if (!s.speedReadSupported) return;
  speedDriveOn = on;
  disp->selectSPI(on ? (benchSpiHz ? benchSpiHz : s.speedSpiHz) : s.spiHz);
  disp->setCutMs(on ? s.speedCutMs : 0);
  if (on) sampleDriveTemp();
  applyDriveHooks(on);
  if (on) logDrive();
}

void setSpeedMode(speed::Mode m) {
  speedModeSet = m;
  if (speedDriveOn && disp && panel::speedReadSupported()) {
    sampleDriveTemp();
    applyDriveHooks(true);
    logDrive();
  }
}

speed::Mode speedMode() { return speedModeSet; }

const speed::Profile *activeSpeedProfile() {
  const panel::Spec &s = panel::activeSpec();
  if (speedModeSet == speed::Mode::SUPER && s.speedSuper) return s.speedSuper;
  // Super Turbo on a panel without it falls back to Turbo.
  if (speedModeSet != speed::Mode::NORMAL && s.speedTurbo) return s.speedTurbo;
  return s.speedNormal;
}

bool speedHooksActive() {
  return disp && panel::speedReadSupported() && disp->hasSpeedHooks();
}

void setSpeedCut(int ms) {
  if (!panel::speedReadSupported()) return;
  disp->setCutMs(ms);
}

void setSpeedPush(bool on) {
  if (!panel::speedReadSupported()) return;
  disp->setDriveAll(on);
}

void benchSetKeepPower(bool on) {
  benchKeepPower = on;
  ovKeepPower = true;
  if (speedDriveOn) disp->setKeepPower(on);
}

void benchSetSingleWrite(bool on) {
  benchSingleWrite = on;
  ovSingleWrite = true;
  if (speedDriveOn) disp->setSingleWrite(on);
}

void benchSetTemp(int t) {
  benchTemp = t < 0 ? -1 : t;
  if (speedDriveOn) disp->setForcedTemp(benchTemp);
}

void benchSetCustomLut(const uint8_t *lut, size_t len) {
  if (!panel::speedReadSupported()) return;
  benchLut = lut;
  benchLutLen = lut ? len : 0;
  ovLut = true;
  benchLutFromSpec = false;
  // Through applyLutAndSkip so a profile's skip previous follows the LUT.
  if (speedDriveOn) applyLutAndSkip(true);
}

void benchSetLut(speed::LutKind kind, int frames, int frameRate, bool pol) {
  if (!panel::speedReadSupported()) return;
  benchLutSpec = {kind, frames, frameRate, pol};
  ovLut = true;
  benchLutFromSpec = true;
  if (speedDriveOn) applyLutAndSkip(true);
}

void benchSetSourceLevels(uint8_t vsh1, uint8_t vsh2, uint8_t vsl) {
  if (!panel::speedReadSupported()) return;
  benchVsh1 = vsh1;
  benchVsh2 = vsh2;
  benchVsl = vsl;
  if (speedDriveOn) disp->setSourceLevels(vsh1, vsh2, vsl);
}

void benchSetSkipPrevious(bool on) {
  if (!panel::speedReadSupported()) return;
  benchSkipPrevious = on;
  ovSkipPrevious = true;
  if (speedDriveOn) disp->setSkipPrevious(on);
}

void benchSetSpiHz(uint32_t hz) {
  if (!panel::speedReadSupported()) return;
  benchSpiHz = hz;
  if (speedDriveOn) disp->selectSPI(hz ? hz : panel::activeSpec().speedSpiHz);
}

void benchSetBulkSpi(bool on) {
  if (disp) disp->setBulkSpi(on);
}

void benchSetLeanTiming(bool on) {
  benchLean = on;
  ovLean = true;
  if (disp && speedDriveOn) disp->setLeanTiming(on);
}

void benchSetProfileLog(bool on) {
  if (disp) disp->setProfileLog(on);
}

void benchClearOverrides() {
  benchKeepPower = benchSingleWrite = benchSkipPrevious = benchLean = false;
  ovKeepPower = ovSingleWrite = ovSkipPrevious = ovLean = false;
  ovLut = false;
  benchLutFromSpec = false;
  benchLut = nullptr;
  benchLutLen = 0;
  benchVsh1 = benchVsh2 = benchVsl = 0;
  benchSpiHz = 0;
  benchTemp = INT_MIN;
  if (speedDriveOn && disp && panel::speedReadSupported()) {
    disp->selectSPI(panel::activeSpec().speedSpiHz);
    applyDriveHooks(true);  // keeps the temperature sampled at drive on
  }
}

BenchDrive benchDrive() {
  BenchDrive d;
  const speed::Profile *p = activeSpeedProfile();
  d.speedDrive = speedDriveOn;
  d.bulkSpi = disp && disp->bulkSpi();
  // Outside speed read the driver runs stock timing; report the setting.
  d.leanTiming = disp && (speedDriveOn ? disp->leanTiming() : effLean(p) && disp->hasSpeedHooks());
  d.keepPower = effKeepPower(p);
  d.singleWrite = effSingleWrite(p);
  d.tempOverridden = benchTemp != INT_MIN;
  d.temp = effForcedTemp(p);
  d.mode = speedModeSet;
  d.hasProfile = p != nullptr;
  d.profile = p ? p->name : "";
  // What speed read runs with (now, or at the next drive on, with the
  // temperature last sampled).
  const LutPlan l = planLut();
  d.lutKind = l.spec.kind;
  d.lutRaw = l.raw;
  d.frames = l.spec.frames;
  d.frameRate = l.spec.frameRate;
  d.tempC = driveTempC;
  d.skipPrevious = l.skip;
  d.leanSet = effLean(p);
  d.ovKeepPower = ovKeepPower;
  d.ovSingleWrite = ovSingleWrite;
  d.ovLean = ovLean;
  d.ovSkipPrevious = ovSkipPrevious;
  d.ovLut = ovLut;
  d.ovTemp = benchTemp != INT_MIN;
  return d;
}

void frameBegin(bool forceFull) {
  beginFrame(forceFull || lastFrameWasImage);
  firstPageClear();
}

void frameBeginRaw(bool full) {
  disp->setFastRefresh(!full);
  if (full) {
    disp->setFullWindow();
  } else {
    // Whole screen, for the same reason as in beginFrame.
    disp->setPartialWindow(0, 0, disp->width(), disp->height());
  }
  firstPageClear();
}

void frameBeginRawKeep() {
  disp->setFastRefresh(true);
  // Whole screen, as in frameBeginRaw. Same addressing as the full window on
  // a landscape canvas (both are the native frame), so the kept bits stay put.
  disp->setPartialWindow(0, 0, disp->width(), disp->height());
}

bool windowFrames() { return disp && disp->honoursWindow(); }

void frameBeginRawWindow(int y, int h) {
  disp->setFastRefresh(true);
  disp->setPartialWindow(0, (uint16_t)y, disp->width(), (uint16_t)h);
  firstPageClear();
}

uint32_t frameEpoch() { return bufferEpoch; }

namespace {

// The view blitText draws through: an addressable buffer at rotation 0 or 2,
// whose canvas is the native frame (so width() is the raw width).
bool blitView(panel::FrameView &v, bool &flip) {
  if (!disp || !disp->frameView(v)) return false;
  const uint8_t r = disp->getRotation();
  if (r != 0 && r != 2) return false;
  if (disp->width() != v.nativeW || disp->height() != v.nativeH) return false;
  flip = r == 2;
  return true;
}

}  // namespace

bool blitAvailable() {
  panel::FrameView v;
  bool flip;
  return blitView(v, flip);
}

// Full-width rows are contiguous bytes in either rotation (rotation 2 only
// reverses their order), so a white fill is one memset over the window's
// share of them. White is a set bit (see blitText).
bool fillRowsWhite(int y, int h) {
  panel::FrameView v;
  bool flip = false;
  if (!blitView(v, flip)) return false;
  if (v.winX != 0 || v.winW != v.nativeW) return false;
  if (h <= 0) return true;
  int r0 = flip ? v.nativeH - (y + h) : y;
  int r1 = r0 + h;
  r0 -= v.winY;
  r1 -= v.winY;
  if (r0 < 0) r0 = 0;
  if (r1 > v.winH) r1 = v.winH;
  if (r1 > r0) memset(v.buf + (size_t)r0 * v.stride, 0xFF, (size_t)(r1 - r0) * v.stride);
  return true;
}

bool frameFlipped() {
  panel::FrameView v;
  bool flip = false;
  return blitView(v, flip) && flip;
}

bool frameBytes(uint8_t *&buf, size_t &len) {
  panel::FrameView v;
  bool flip;
  if (!blitView(v, flip)) return false;
  buf = v.buf;
  len = (size_t)v.stride * v.winH;
  return true;
}

// Mirrors Adafruit_GFX::write(uint8_t) for a custom font (text size 1, wrap
// off) and drawChar's glyph loop: glyph bits run on across rows, MSB first,
// and a set bit is the pixel at (cursor + xOffset + xx, cursor + yOffset +
// yy), written as GxEPD2_BW::drawPixel would write it. Instead of testing
// every pixel against the canvas and the window, each glyph's columns and
// rows are cut to the range that passes both tests, and the bit index into
// the glyph bitmap is computed for every pixel, so clipped pixels still
// consume their bits.
bool blitText(const GFXfont *font, int x, int y, const char *s, uint16_t color,
              int &endX, int &endY) {
  panel::FrameView v;
  bool flip = false;
  if (!font || !s || !blitView(v, flip)) return false;
  const int W = v.nativeW, H = v.nativeH;
  // Canvas x and y (half open) that land inside both the canvas and the
  // window once turned to native coordinates.
  int xLo, xHi, yLo, yHi;
  if (!flip) {
    xLo = v.winX;
    xHi = v.winX + v.winW;
    yLo = v.winY;
    yHi = v.winY + v.winH;
  } else {
    xLo = W - v.winX - v.winW;
    xHi = W - v.winX;
    yLo = H - v.winY - v.winH;
    yHi = H - v.winY;
  }
  xLo = std::max(xLo, 0);
  xHi = std::min(xHi, W);
  yLo = std::max(yLo, 0);
  yHi = std::min(yHi, H);
  const uint8_t *bitmap = font->bitmap;
  const uint8_t first = (uint8_t)font->first;
  const uint8_t last = (uint8_t)font->last;
  const bool white = color != 0;
  int16_t cx = (int16_t)x, cy = (int16_t)y;
  for (const uint8_t *p = (const uint8_t *)s; *p; p++) {
    const uint8_t c = *p;
    if (c == '\n') {
      cx = 0;
      cy += (int16_t)font->yAdvance;
      continue;
    }
    if (c == '\r' || c < first || c > last) continue;
    const GFXglyph &gl = font->glyph[c - first];
    const int w = gl.width, h = gl.height;
    if (w > 0 && h > 0) {
      const int gx = cx + gl.xOffset;
      const int gy = cy + gl.yOffset;
      const int xa = std::max(0, xLo - gx), xb = std::min(w, xHi - gx);
      const int ya = std::max(0, yLo - gy), yb = std::min(h, yHi - gy);
      const uint8_t *bits = bitmap + gl.bitmapOffset;
      for (int yy = ya; yy < yb; yy++) {
        const int cyy = gy + yy;
        const int by = (flip ? H - 1 - cyy : cyy) - v.winY;
        uint8_t *row = v.buf + (size_t)by * v.stride;
        uint32_t bit = (uint32_t)yy * w + xa;
        for (int xx = xa; xx < xb; xx++, bit++) {
          if (!(bits[bit >> 3] & (0x80 >> (bit & 7)))) continue;
          const int cxx = gx + xx;
          const int bx = (flip ? W - 1 - cxx : cxx) - v.winX;
          const uint8_t m = 0x80 >> (bx & 7);
          if (white) {
            row[bx >> 3] |= m;
          } else {
            row[bx >> 3] &= (uint8_t)~m;
          }
        }
      }
    }
    cx += (int16_t)gl.xAdvance;
  }
  endX = cx;
  endY = cy;
  return true;
}

bool frameNext() { return disp->nextPage(); }

void frameEnd() { lastFrameWasImage = false; }

bool beginPanel(uint8_t id) {
  // Old driver and frame buffer go first, so two buffers never coexist.
  if (disp) {
    panel::destroy(disp);
    disp = nullptr;
  }
  bufferEpoch++;
  panel::use(id);
  disp = panel::activeSpec().create();
  bool ok = disp != nullptr;
  if (!ok && panel::active() != 0) {
    Serial.printf("panel %u: display alloc failed, falling back to panel 0\n",
                  (unsigned)id);
    panel::use(0);
    disp = panel::activeSpec().create();
  }
  if (!disp) {
    // Not even panel 0's buffer fits: there is nothing left to draw with.
    // This runs before WiFi and the filesystem, so it cannot happen on a
    // healthy board; panic and let the reboot have another go.
    Serial.printf("display alloc failed, %u free, %u largest block\n",
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
    abort();
  }
  panel::applyGeometry();
  const panel::Spec &spec = panel::activeSpec();

  disp->setBusyCallback(feedWatchdogWhileBusy);
  // SPI clock and reset pulse come from the panel table. Panel 0 runs 10 MHz
  // (the stock 4 MHz plus per byte transfers cost over half a second per
  // refresh on its 156 KB two plane frame; the driver writes whole rows in one
  // burst, so drop back to 4 MHz in the table if the panel ever shows streaks)
  // and a 50 ms reset pulse, the same value the Muon firmware uses: the
  // SSD2677 does not come up reliably on the 2 ms pulse the smaller Waveshare
  // panels are happy with.
  disp->selectSPI(spec.spiHz);
  disp->init(115200, true, spec.resetMs, false);
  // The panel's UI rotation: odd (portrait) on panels 0 and 1, whose native
  // frame is turned on its side, even (landscape) on panel 2, which keeps it.
  // Either way it gives the canvas every layout global in config.h was
  // computed for. Flip the panel's rotation in src/panel.cpp by 2 (1 <-> 3,
  // 0 <-> 2) if the image comes up upside down.
  disp->setRotation(DISPLAY_ROTATION);
  portraitRotation = DISPLAY_ROTATION;
  landscape = false;
  u8g2.begin(canvas());
  u8g2.setFontMode(1);
  u8g2.setFontDirection(0);
  setInk(false);
  // A new driver knows nothing about what is on glass. The ration starts
  // over and the next text frame clears whatever a previous panel left.
  partialsSinceFull = 0;
  lastFrameWasImage = false;

  // Heap budget check. Panel 0's partial-refresh driver malloc's a shadow of
  // the previous frame (one more frame's worth of bytes) on its first write,
  // on top of the frame buffer allocated above. A news sync then wants
  // roughly 50 KB more for TLS on top of the WiFi stack, so this line is how
  // you confirm on the bench that the reader still has room for both.
  Serial.printf("panel %u (%s): heap after display init: %u free, %u largest "
                "block (frame %u bytes)\n",
                (unsigned)panel::active(), spec.name,
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap(),
                (unsigned)(((spec.nativeW + 7) / 8) * spec.nativeH));
  return ok;
}

void begin() {
  SPI.begin(PIN_EINK_SCK, PIN_EINK_MISO, PIN_EINK_MOSI, PIN_EINK_CS);

  {
    // Wiring diagnostic: a floating BUSY follows whichever pull we apply.
    pinMode(PIN_EINK_BUSY, INPUT_PULLUP); delay(2); int up = digitalRead(PIN_EINK_BUSY);
    pinMode(PIN_EINK_BUSY, INPUT_PULLDOWN); delay(2); int dn = digitalRead(PIN_EINK_BUSY);
    pinMode(PIN_EINK_BUSY, INPUT);
    pinMode(PIN_EINK_RST, OUTPUT); digitalWrite(PIN_EINK_RST, LOW); delay(50);
    digitalWrite(PIN_EINK_RST, HIGH); delay(50);
    int afterRst = digitalRead(PIN_EINK_BUSY);
    Serial.printf("BUSY diag: pullup=%d pulldown=%d afterReset=%d (%s)\n", up, dn, afterRst,
                  (up != dn) ? "FLOATING, BUSY wire open" : (afterRst ? "driven HIGH" : "driven LOW"));
  }

  // The stored panel choice (missing = panel 0), then its geometry and
  // driver. setup() calls this first thing, so the frame buffer comes out of
  // an unfragmented heap.
  beginPanel(panel::load());
}

void renderPage(const String &title, const String &pageText, uint8_t fontId,
                uint32_t pageIndex, uint32_t pageCount, bool forceFull) {
  String status = "page " + String((unsigned long)(pageIndex + 1)) + "/" +
                  String((unsigned long)pageCount);
  const TextGrid grid = gridFor(fontId);

  beginFrame(forceFull || lastFrameWasImage);
  firstPageClear();
  do {
    disp->fillScreen(GxEPD_WHITE);
    u8g2.setFont(grid.font);
    setInk(false);

    int row = 0;
    int start = 0;
    while (row < grid.rows && start <= (int)pageText.length()) {
      int nl = pageText.indexOf('\n', start);
      String line = (nl < 0) ? pageText.substring(start)
                             : pageText.substring(start, nl);
      line.replace("\r", "");
      if (line.length() > (unsigned)grid.cols) line = line.substring(0, grid.cols);
      u8g2.setCursor(TEXT_MARGIN_X, grid.firstBaselineY + row * grid.pitch);
      u8g2.print(line);
      row++;
      if (nl < 0) break;
      start = nl + 1;
    }

    drawStatusStrip(title, status, true);
  } while (disp->nextPage());
  lastFrameWasImage = false;
}

void renderImagePage(const String &title, const uint8_t *bits, uint16_t width,
                     uint16_t height, uint8_t flags, uint32_t pageIndex,
                     uint32_t pageCount) {
  String status = "page " + String((unsigned long)(pageIndex + 1)) + "/" +
                  String((unsigned long)pageCount);
  const bool doubled = (flags & MPG1_IMAGE_FLAG_DOUBLE) != 0;
  const int scale = doubled ? 2 : 1;
  const int srcStride = (width + 7) / 8;

  // Content area is everything above the status rule, full width.
  const int areaW = CONTENT_W;
  const int areaH = CONTENT_H;
  int drawW = (int)width * scale;
  int drawH = (int)height * scale;

  // A page bigger than the content area (a book converted for a larger panel)
  // is scaled down to fit, aspect kept, nearest neighbour straight from the
  // stored pixels, and centred like any other page. A page that fits takes
  // the unscaled paths below, unchanged.
  const bool shrink = bits != nullptr && width > 0 && height > 0 &&
                      (drawW > areaW || drawH > areaH);
  if (shrink) {
    // Width bound when drawW/drawH is wider than areaW/areaH.
    if ((long)drawW * areaH > (long)drawH * areaW) {
      drawH = (int)((long)drawH * areaW / drawW);
      drawW = areaW;
    } else {
      drawW = (int)((long)drawW * areaH / drawH);
      drawH = areaH;
    }
    if (drawW < 1) drawW = 1;
    if (drawH < 1) drawH = 1;
  }

  int x0 = (disp->width() - drawW) / 2;
  int y0 = (areaH - drawH) / 2;
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  // Rows that would run under the status strip are dropped rather than drawn.
  int visibleRows = height;
  if (!shrink && y0 + drawH > areaH) visibleRows = (areaH - y0) / scale;
  if (visibleRows < 0) visibleRows = 0;

  // Row expansion buffer. Doubled case: one stored row widened to 2x and held
  // twice, so a single drawBitmap call paints both output rows. Shrink case:
  // one output row.
  const int dstStride = (drawW + 7) / 8;
  std::vector<uint8_t> rowBuf;
  if (shrink) {
    rowBuf.assign((size_t)dstStride, 0);
  } else if (doubled) {
    rowBuf.assign((size_t)dstStride * 2, 0);
  }

  // Always a full refresh: dithered art under a partial refresh ghosts badly.
  beginFrame(true);
  firstPageClear();
  do {
    disp->fillScreen(GxEPD_WHITE);

    if (shrink) {
      for (int oy = 0; oy < drawH; oy++) {
        const int sy = (int)((long)oy * height / drawH);
        const uint8_t *src = bits + (size_t)sy * srcStride;
        std::fill(rowBuf.begin(), rowBuf.end(), 0);
        for (int ox = 0; ox < drawW; ox++) {
          const int sx = (int)((long)ox * width / drawW);
          if (src[sx >> 3] & (0x80 >> (sx & 7))) {
            rowBuf[ox >> 3] |= (uint8_t)(0x80 >> (ox & 7));
          }
        }
        canvas().drawBitmap(x0, y0 + oy, rowBuf.data(), drawW, 1,
                            GxEPD_BLACK);
      }
    } else if (bits != nullptr && visibleRows > 0) {
      if (!doubled) {
        canvas().drawBitmap(x0, y0, bits, width, visibleRows, GxEPD_BLACK);
      } else {
        for (int y = 0; y < visibleRows; y++) {
          const uint8_t *src = bits + (size_t)y * srcStride;
          std::fill(rowBuf.begin(), rowBuf.end(), 0);
          for (int x = 0; x < (int)width; x++) {
            if (!(src[x >> 3] & (0x80 >> (x & 7)))) continue;
            int dx = x * 2;
            rowBuf[dx >> 3] |= (uint8_t)(0x80 >> (dx & 7));
            dx++;
            rowBuf[dx >> 3] |= (uint8_t)(0x80 >> (dx & 7));
          }
          // Second output row is a copy of the first.
          std::copy(rowBuf.begin(), rowBuf.begin() + dstStride,
                    rowBuf.begin() + dstStride);
          canvas().drawBitmap(x0, y0 + y * 2, rowBuf.data(), drawW, 2,
                              GxEPD_BLACK);
        }
      }
    }

    setInk(false);
    drawStatusStrip(title, status, true);
  } while (disp->nextPage());
  lastFrameWasImage = true;
}

int menuColumns() { return menuCols(); }

void renderMenu(const String &header, const std::vector<MenuTile> &tiles,
                int cursor, bool forceFull) {
  const int count = (int)tiles.size();
  const int cols = menuCols();
  const int rowsShown = menuRows();
  const int tileW = menuTileW();
  const int tileH = menuTileH();
  const int gridTop = menuGridTop();
  const int totalRows = (count + cols - 1) / cols;
  if (cursor < 0) cursor = 0;
  if (cursor >= count) cursor = count - 1;
  // Scroll by whole rows, only as far as it takes to show the cursor's row.
  const int cursorRow = (cursor >= 0) ? cursor / cols : 0;
  int firstRow = cursorRow - rowsShown + 1;
  if (firstRow < 0) firstRow = 0;

  String footerLeft = "menu";
  if (totalRows > rowsShown) {
    int lastShown = firstRow + rowsShown;
    if (lastShown > totalRows) lastShown = totalRows;
    footerLeft += "  rows " + String(firstRow + 1) + "-" + String(lastShown) +
                  " of " + String(totalRows);
  }

  // The menu is entered with a full refresh in most cases (lastFrameWasImage
  // or the caller's forceFull); cursor moves ride the partial LUT and the
  // ration in beginFrame clears the ghost the inverted tile leaves behind.
  beginFrame(forceFull || lastFrameWasImage);
  firstPageClear();
  do {
    disp->fillScreen(GxEPD_WHITE);

    // Header band: solid black, MENU tag plus the sensor line in white.
    canvas().fillRect(0, 0, disp->width(), ui::headerH(), GxEPD_BLACK);
    setInk(true);
    u8g2.setFont(FONT_STATUS_RIGHT_BOLD);
    u8g2.setCursor(TEXT_MARGIN_X, ui::headerBaselineY());
    u8g2.print("MENU");
    int hx = TEXT_MARGIN_X + u8g2.getUTF8Width("MENU") + 28;
    u8g2.setFont(FONT_STATUS_RIGHT);
    u8g2.setCursor(hx, ui::headerBaselineY());
    u8g2.print(fitText(header, disp->width() - TEXT_MARGIN_X - hx));

    // Tiles.
    for (int i = 0; i < count; i++) {
      int r = i / cols - firstRow;
      if (r < 0) continue;
      if (r >= rowsShown) break;
      int c = i % cols;
      int x = TEXT_MARGIN_X + c * (tileW + MENU_GUTTER);
      int y = gridTop + r * (tileH + MENU_GUTTER);
      drawMenuTile(x, y, i + 1, tiles[i], i == cursor);
    }

    setInk(false);
    // The founder's "right" button is BTN_LEFT in the enum; the hint names
    // the button as held, not as wired.
    drawStatusStrip(footerLeft, "DOWN row   RIGHT column   CENTER pick");
  } while (disp->nextPage());
  lastFrameWasImage = false;
}

void renderJump(const String &title, uint32_t target, uint32_t pageCount,
                bool forceFull) {
  beginFrame(forceFull || lastFrameWasImage);
  firstPageClear();
  do {
    disp->fillScreen(GxEPD_WHITE);

    u8g2.setFont(FONT_BODY);
    setInk(false);
    u8g2.setCursor(TEXT_MARGIN_X, TEXT_FIRST_BASELINE_Y);
    u8g2.print("Jump to page");

    // Big number on the vertical midline of the canvas, with the "of N" line
    // and the help line spaced below it. All three derive from SCREEN_H so the
    // block re-centres on every panel. On a canvas too short for that spacing
    // (the 272 px bar: the help lines would land at 316 and 340, past the
    // rule at 248) the block closes up around the middle of the content area
    // instead: number 134, "of N" 178, help 222 (descenders end at 226).
    int centerY = SCREEN_H / 2;
    int ofBaselineY = centerY + 70;
    int helpBaselineY = centerY + 180;
    const bool tight =
        helpBaselineY + TEXT_LINE_PITCH + TEXT_DESCENT >= STATUS_RULE_Y;
    if (tight) {
      centerY = STATUS_RULE_Y / 2 + 10;
      ofBaselineY = centerY + 44;
      helpBaselineY = centerY + 88;
    }

    String num = String((unsigned long)target);
    u8g2.setFont(FONT_BIG);
    int w = u8g2.getUTF8Width(num.c_str());
    u8g2.setCursor((disp->width() - w) / 2, centerY);
    u8g2.print(num);

    u8g2.setFont(FONT_BODY);
    String of = "of " + String((unsigned long)pageCount);
    int w2 = u8g2.getUTF8Width(of.c_str());
    u8g2.setCursor((disp->width() - w2) / 2, ofBaselineY);
    u8g2.print(of);

    // Two lines: the portrait grid is too narrow for one. A short canvas has
    // room for one line only, and is wide enough for it.
    String help1 = "RIGHT +1  LEFT -1";
    String help2 = "UP +10  DOWN -10  CENTER go";
    if (tight) {
      help1 += "  " + help2;
      help2 = "";
    }
    int w3 = u8g2.getUTF8Width(help1.c_str());
    u8g2.setCursor((disp->width() - w3) / 2, helpBaselineY);
    u8g2.print(help1);
    int w4 = u8g2.getUTF8Width(help2.c_str());
    u8g2.setCursor((disp->width() - w4) / 2, helpBaselineY + TEXT_LINE_PITCH);
    u8g2.print(help2);

    drawStatusStrip(title, "jump");
  } while (disp->nextPage());
  lastFrameWasImage = false;
}

void renderMessage(const String &title, const std::vector<String> &lines) {
  renderStatusScreen(title, lines, true);
}

// Small form of a status screen, for more lines than the body grid has rows:
// 7x14 at a 16 px pitch from baseline 22 (caps from 12, like the body's), so
// (STATUS_RULE_Y - 1 - 3 - 22) / 16 + 1 rows a column: 14 on the 5.79 inch
// bar against its 10 body rows. Columns are STATUS_COL_GUTTER apart and share
// the width, two of (760 - 24) / 2 = 368 px (52 characters) there.
static const int STATUS_SMALL_FIRST_BASELINE_Y = 22;
static const int STATUS_SMALL_PITCH = 16;
static const int STATUS_SMALL_DESCENT = 3;
static const int STATUS_COL_GUTTER = 24;

// Splits `lines` into columns of at most `perCol`. A column ends early on a
// blank line in its second half, so a paragraph is not cut across columns,
// unless that would need more columns than a plain split; a blank line left
// at the top of a column is dropped. Each column is {first, end}.
std::vector<std::pair<int, int>> statusColumns(const std::vector<String> &lines,
                                               int perCol) {
  const int n = (int)lines.size();
  auto split = [&](bool atBlank) {
    std::vector<std::pair<int, int>> cols;
    int i = 0;
    while (i < n) {
      while (i < n && !cols.empty() && lines[i].length() == 0) i++;
      if (i >= n) break;
      int end = std::min(i + perCol, n);
      if (atBlank && end < n) {
        for (int b = end - 1; b > i + perCol / 2; b--) {
          if (lines[b].length() == 0) {
            end = b;
            break;
          }
        }
      }
      cols.push_back(std::make_pair(i, end));
      i = end;
    }
    return cols;
  };
  std::vector<std::pair<int, int>> plain = split(false);
  std::vector<std::pair<int, int>> tidy = split(true);
  return tidy.size() <= plain.size() ? tidy : plain;
}

void renderStatusScreen(const String &title, const std::vector<String> &lines,
                        bool forceFull) {
  const bool small = (int)lines.size() > TEXT_ROWS;
  std::vector<std::pair<int, int>> cols;
  int colW = SCREEN_W - 2 * TEXT_MARGIN_X;
  if (small) {
    int perCol = (STATUS_RULE_Y - 1 - STATUS_SMALL_DESCENT -
                  STATUS_SMALL_FIRST_BASELINE_Y) / STATUS_SMALL_PITCH + 1;
    if (perCol < 1) perCol = 1;
    cols = statusColumns(lines, perCol);
    const int n = cols.empty() ? 1 : (int)cols.size();
    colW = (SCREEN_W - 2 * TEXT_MARGIN_X - STATUS_COL_GUTTER * (n - 1)) / n;
  }

  beginFrame(forceFull || lastFrameWasImage);
  firstPageClear();
  do {
    disp->fillScreen(GxEPD_WHITE);
    setInk(false);
    if (!small) {
      u8g2.setFont(FONT_BODY);
      for (int i = 0; i < (int)lines.size() && i < TEXT_ROWS; i++) {
        u8g2.setCursor(TEXT_MARGIN_X,
                       TEXT_FIRST_BASELINE_Y + i * TEXT_LINE_PITCH);
        // Clipped, not wrapped: an SSID or IP line can outrun the narrower
        // grid of a larger, lower resolution panel.
        u8g2.print(fitText(lines[i], SCREEN_W - 2 * TEXT_MARGIN_X));
      }
    } else {
      u8g2.setFont(FONT_STATUS_RIGHT);
      for (size_t c = 0; c < cols.size(); c++) {
        const int x = TEXT_MARGIN_X + (int)c * (colW + STATUS_COL_GUTTER);
        for (int i = cols[c].first; i < cols[c].second; i++) {
          u8g2.setCursor(x, STATUS_SMALL_FIRST_BASELINE_Y +
                                (i - cols[c].first) * STATUS_SMALL_PITCH);
          u8g2.print(fitText(lines[i], colW));
        }
      }
    }
    drawStatusStrip(title, "");
  } while (disp->nextPage());
  lastFrameWasImage = false;
}

}  // namespace ui
