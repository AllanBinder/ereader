#include "rsvp.h"

#include <Preferences.h>

#include <vector>

#include "Gelasio28pt7b.h"
#include "Gelasio34pt7b.h"
#include "Gelasio40pt7b.h"
#include "Gelasio48pt7b.h"
#include "config.h"
#include "panel.h"
#include "speed_profile.h"
#include "ui.h"

namespace {

void logf(const char *fmt, ...) {
  char buf[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  Serial.print(buf);
}

// ------------------------------------------------------------------ layout
//
// Landscape, the panel's native frame. Two layouts, picked from the canvas
// height (not by naming a panel):
//  - tall (h >= SHORT_BELOW): 920x680 on the 5.76 inch panel. The words band
//    is centred on the height and the footer is anchored to the bottom edge,
//    with big HUD_NUM readouts for rate, page and time left. Header, band and
//    footer together need about 680 px; a 480 px frame (the 7.5 inch panel)
//    would put the readouts on the band, so it needs its own pass before its
//    row in src/panel.cpp turns speed read on.
//  - short (h < SHORT_BELOW): the 792x272 wide bar (panel 2). One header line
//    carries the rate and page, the band is smaller and the word sizes are
//    Gelasio 34/28 (the bar's pixels are about 1.37x larger), and the footer
//    is a progress bar and a status line; the y budget is at "short layout"
//    below. RULE_ABOVE / RULE_BELOW and FONTS are the tall layout's.
// In both, two hairline rules across the width frame the band the
// words flash in, and a status line sits at the bottom. Every chunk starts at
// the same left edge (WORDS_LEFT), so the eye returns to one spot each flash
// and only the chunk's right edge moves. (Anchoring each chunk on its first
// word's Spritz ORP letter moved the left edge by up to a word's width from
// flash to flash, which read as bouncing.) Readers take in about 3 to 4
// characters left of a fixation and 14 to 15 right of it (McConkie and
// Rayner), so a fixation just inside the left edge (FIX_IN, marked by the
// pointers on the rules) covers a 3-word chunk. Everything but the words is
// the same from frame to frame, so the differential refresh never drives it
// (see HUD below).
const int RULE_INSET = 40;
const int RULE_ABOVE = 110;      // top rule this far above the baseline
const int RULE_BELOW = 50;       // bottom rule this far below it
const int WORDS_LEFT = 90;       // left edge of every chunk
const int WORDS_EDGE = 30;       // a chunk must stay this far inside the right edge
const int FIX_IN = 40;           // fixation pointers, this far right of WORDS_LEFT
const int TICK_LEN = 18;         // fixation ticks hanging off the rules

// One working size (FONT_BASE) so the letters do not change size from flash
// to flash; a chunk steps down only when it would run off an edge. 48pt is
// kept for a future size setting.
const GFXfont *const FONTS[] = {&Gelasio48pt7b, &Gelasio40pt7b, &Gelasio34pt7b,
                                &Gelasio28pt7b};
const int FONT_BASE = 1;
const int FONT_COUNT = sizeof(FONTS) / sizeof(FONTS[0]);

// Short layout: canvases under SHORT_BELOW px tall. Its own fonts (34pt
// base, 28pt fallback) and its own fixed y positions, worked out in
// composeShort.
const int SHORT_BELOW = 480;
const GFXfont *const FONTS_SHORT[] = {&Gelasio34pt7b, &Gelasio28pt7b};
const int FONT_SHORT_COUNT = sizeof(FONTS_SHORT) / sizeof(FONTS_SHORT[0]);

bool isShort(Adafruit_GFX &g) { return g.height() < SHORT_BELOW; }

// --------------------------------------------------------------- the words
//
// A page's text is its lines joined with spaces into words. The paginator
// (tools/pdf2book.py paginate_spans, and its twin src/text_paginate.cpp) puts
// exactly one blank line between paragraphs and never one at the top of a
// page, so a blank line inside a page is a paragraph break. A paragraph that
// ends on the last line of a page leaves no blank line behind, so the page
// end is decided from the greedy wrap instead: the wrap only moves a word to a
// new line when it does not fit, so if the next page's first word would have
// fit after the page's last line, the paragraph must have ended there.
struct Word {
  String text;
  bool paraEnd;  // a blank line follows it on the same page
};

struct PageWords {
  uint32_t page = UINT32_MAX;  // UINT32_MAX = empty slot
  std::vector<Word> words;     // empty for image pages and blank pages
  int firstWordCols = 0;       // characters in the first word, as paginated
  int lastLineCols = 0;        // characters on the last non blank line
  bool endsBlank = false;      // the page's last line is blank
};

// A few pages are enough: a chunk spans at most two pages, and deciding a
// page end looks at the next one.
const int CACHE_SLOTS = 3;
PageWords cache[CACHE_SLOTS];
int cacheNext = 0;

struct Pos {
  uint32_t page = 0;
  uint16_t word = 0;
};

Book *bk = nullptr;

// Settings and playback state.
int wpmValue = RSVP_WPM_DEFAULT;
int wordsPerFlash = RSVP_WORDS_DEFAULT;
bool playing = false;
bool ended = false;          // ran off the end of the book
bool havePos = false;        // `pos` is a word (false at the end)
Pos pos;                     // first word of the next chunk
Pos shown;                   // first word of the chunk on glass
String current;              // text on glass, empty for the blank band
int currentWords = 0;        // words in `current`
float currentDwell = 1.0f;   // hold multiplier for the chunk on glass
uint32_t curPage = 0;        // reading position, the page of `shown`
int partialsSinceFull = 0;
int cutsSincePush = 0;       // cut flashes since the last push or full
bool fullPending = false;    // clear ghosts at the next due time
// Soft clear (Turbo, 2026-11-09): the ghost clearing full is replaced by two
// drive all partials, the whole screen black, then the blank band. On the
// founder's unit a real full refresh left negatives of the next few words
// under the NOHOLD LUT, which white pushes did not fix. Entry frames stay
// full. Console `sclear 0|1`.
bool benchSoftClear = true;
bool inkFrame = false;       // compose() fills black (soft clear)
// A real full still runs about every 10 minutes of Turbo reading (the panel's
// own temperature tuned waveform resets slow build up the short LUT can
// leave), at the next ghost clear due, followed by a soft clear.
const uint32_t RSVP_REAL_FULL_EVERY_MS = 10UL * 60UL * 1000UL;
uint32_t lastRealFullAt = 0;
bool softClearOn();  // below, with tick()
void softClear();
uint32_t nextDue = 0;
bool active = false;         // between enter() and leave()

// Bench console (src/bench.cpp) state. All of it stays idle unless a console
// command set it, so the reader behaves as without the console.
bool benchUncapped = false;  // a console `wpm` set a speed past the panel's top
bool benchPushNext = false;  // white push on the next partial, then off
bool benchLog = false;       // one serial line per playback flash
uint32_t lastFrameMs = 0;    // frameBeginRaw .. frameEnd of the last render()
uint32_t lastComposeMs = 0;  // of which inside compose (the rest is transfer + refresh)
uint32_t lastGapMs = 0;      // previous frame's end to this frame's start
uint32_t lastFrameEndAt = 0; // millis() at the end of the last render()

// How render() built the frame: the whole screen composed from scratch, only
// the words band redrawn over the kept buffer, or only the band's rows sent
// in a partial window. See render().
enum class Compose : uint8_t { FULL, BAND, WINDOW };
Compose lastCompose = Compose::FULL;
bool benchInc = true;   // incremental compose (BAND) allowed
bool benchWin = false;  // band window (WINDOW) allowed
// Glyph blit: the chunk's words drawn straight into the frame buffer bytes
// (ui::blitText) instead of one virtual drawPixel per set pixel, bit for bit
// the same frame. Falls back to the GFX path wherever blitText says no (a
// driver whose buffer is not addressable, a portrait rotation).
bool benchBlit = true;
bool blitUsed = false;          // the last drawWords went through the blit
uint32_t lastFrameStartAt = 0;  // millis() at the start of the last render()

// Rolling timing of the last STAT_SLOTS playback flashes (tick() partials;
// entry, ghost clearing, pause and rewind frames are not counted).
const int STAT_SLOTS = 32;
const uint32_t LATE_SLIP_MS = 20;  // started this much past due = late
struct FlashStat {
  uint32_t at;  // millis() at the frame's start
  uint16_t frameMs;
  uint16_t composeMs;
  uint16_t gapMs;
  uint16_t holdMs;  // the hold it was given (dwell included)
  uint8_t words;    // words in the chunk
  bool late;
  bool blit;        // its words went through the glyph blit
  bool afterFull;   // a full refresh ran between the previous flash and this one
  Compose compose;
};
FlashStat stats[STAT_SLOTS];
int statCount = 0;  // valid slots, up to STAT_SLOTS
int statNext = 0;
uint32_t flashNo = 0;  // playback flashes since enter()
const int BENCH_WPM_MAX = 1000;
bool fullSinceFlash = false;  // render(true) ran since the last recordFlash

// Achieved speed on the HUD (realWpm below). Measured over the newest
// playback flashes at the current settings: realFirst is the number of the
// first flash that counts, moved on by a resume (realReset) and by a speed,
// words or mode change (recordFlash).
const int REAL_MIN_INTERVALS = 8;  // flash to flash intervals before it shows
const int REAL_SHOW_PCT = 90;      // shown below this share of the set speed
const int REAL_HIDE_PCT = 93;      // and hidden again at this share or above
uint32_t realFirst = 1;
int realForWpm = 0, realForWords = 0;
speed::Mode realForMode = speed::Mode::NORMAL;
int realShown = 0;  // figure on glass, 0 = none

// Time-left calibration, both seeded with a prior so the first estimate is
// sane. Book text: stream characters per word, measured on every text page
// loaded (anchor gap to the next page vs. the page's word count). Pacing:
// time actually held per flash vs. the nominal words/wpm, which folds in the
// sentence and clause dwell and the short chunks at sentence ends.
double calChars = 6.0 * 400, calWords = 400;
double calHeld = 1.25 * 60, calNominal = 60;

// Where speed read was last left, for resumeWord().
String lastSlug;
uint8_t lastVariant = 0;
uint32_t lastPage = 0;
uint16_t lastWord = 0;

// ------------------------------------------------------------ text helpers

// Next codepoint of UTF-8 `s` at byte `i`, advancing `i`. A malformed byte
// comes back as 0xFFFD, which transliterates to nothing.
uint32_t nextCodepoint(const String &s, int &i, int end) {
  uint8_t b = (uint8_t)s[i++];
  if (b < 0x80) return b;
  int need = (b & 0xE0) == 0xC0 ? 1 : (b & 0xF0) == 0xE0 ? 2 : (b & 0xF8) == 0xF0 ? 3 : -1;
  if (need < 0) return 0xFFFD;
  uint32_t cp = b & (0x3F >> need);
  while (need-- > 0) {
    if (i >= end || ((uint8_t)s[i] & 0xC0) != 0x80) return 0xFFFD;
    cp = (cp << 6) | ((uint8_t)s[i++] & 0x3F);
  }
  return cp;
}

// Latin-1 0xA0..0xFF to the nearest ASCII, "" to drop. The Gelasio fonts are
// 7 bit (0x20..0x7E); book text keeps the Latin-1 supplement.
const char *const LATIN1[96] = {
    " ", "!", "c", "L", "", "Y", "|", "S", "", "(c)", "a", "\"", "-", "", "(R)", "",
    "", "+-", "2", "3", "'", "u", "", ".", "", "1", "o", "\"", "1/4", "1/2", "3/4", "?",
    "A", "A", "A", "A", "A", "A", "AE", "C", "E", "E", "E", "E", "I", "I", "I", "I",
    "D", "N", "O", "O", "O", "O", "O", "x", "O", "U", "U", "U", "U", "Y", "Th", "ss",
    "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",
    "d", "n", "o", "o", "o", "o", "o", "/", "o", "u", "u", "u", "u", "y", "th", "y",
};

// Appends the ASCII form of `cp` to `out`. Curly quotes and dashes are
// normalized by the converters already; they are mapped here too in case a
// book slipped through without it.
void appendAscii(uint32_t cp, String &out) {
  if (cp >= 0x21 && cp <= 0x7E) {
    out += (char)cp;
  } else if (cp >= 0xA1 && cp <= 0xFF) {
    out += LATIN1[cp - 0xA0];
  } else if (cp == 0x2018 || cp == 0x2019 || cp == 0x201A || cp == 0x2032) {
    out += '\'';
  } else if (cp == 0x201C || cp == 0x201D || cp == 0x201E || cp == 0x2033) {
    out += '"';
  } else if (cp >= 0x2010 && cp <= 0x2015) {
    out += '-';
  } else if (cp == 0x2026) {
    out += "...";
  }
}

bool isSeparator(uint32_t cp) { return cp == ' ' || cp == '\t' || cp == 0xA0; }

// Splits one page's text into words and the facts the page end test needs.
void tokenize(const String &text, PageWords &pw) {
  pw.words.clear();
  pw.firstWordCols = 0;
  pw.lastLineCols = 0;
  pw.endsBlank = false;
  bool seenLine = false;
  const int len = text.length();
  int start = 0;
  while (start <= len) {
    int nl = text.indexOf('\n', start);
    int end = (nl < 0) ? len : nl;

    int cols = 0;       // characters on the line, the paginator's count
    int tokCols = 0;    // characters in the token being read
    String word;
    bool blank = true;
    for (int i = start; i < end;) {
      uint32_t cp = nextCodepoint(text, i, end);
      if (cp == '\r') continue;
      cols++;
      if (isSeparator(cp)) {
        if (tokCols && !seenLine && pw.firstWordCols == 0) pw.firstWordCols = tokCols;
        if (word.length()) pw.words.push_back(Word{word, false});
        word = "";
        tokCols = 0;
        continue;
      }
      blank = false;
      tokCols++;
      appendAscii(cp, word);
    }
    if (tokCols && !seenLine && pw.firstWordCols == 0) pw.firstWordCols = tokCols;
    if (word.length()) pw.words.push_back(Word{word, false});

    if (blank) {
      if (!pw.words.empty()) pw.words.back().paraEnd = true;
    } else {
      seenLine = true;
      pw.lastLineCols = cols;
    }
    pw.endsBlank = blank;
    if (nl < 0) break;
    start = nl + 1;
  }
}

// The page's words, from the cache or freshly loaded. The pointer is only
// good until the next call: loading another page may reuse its slot.
const PageWords *load(uint32_t page) {
  for (int i = 0; i < CACHE_SLOTS; i++) {
    if (cache[i].page == page) return &cache[i];
  }
  PageWords &pw = cache[cacheNext];
  cacheNext = (cacheNext + 1) % CACHE_SLOTS;
  pw.page = page;
  // A local PageData, so an image page's raster is freed on return rather
  // than held for the rest of the session.
  PageData data;
  if (bk->loadPage(page, data) && !data.isImage) {
    tokenize(data.text, pw);
    uint32_t a0 = bk->anchorOf(page), a1 = bk->anchorOf(page + 1);
    if (a1 > a0 && !pw.words.empty()) {
      calChars += a1 - a0;
      calWords += pw.words.size();
    }
  } else {
    pw.words.clear();
    pw.firstWordCols = 0;
    pw.lastLineCols = 0;
    pw.endsBlank = true;
  }
  return &pw;
}

void clearCache() {
  for (int i = 0; i < CACHE_SLOTS; i++) {
    cache[i].page = UINT32_MAX;
    cache[i].words.clear();
    cache[i].words.shrink_to_fit();
  }
  cacheNext = 0;
}

size_t wordCount(uint32_t page) { return load(page)->words.size(); }

// Next page after `from` with words on it, skipping image and blank pages.
bool nextTextPage(uint32_t from, uint32_t &out) {
  for (uint32_t q = from + 1; q < bk->pageCount(); q++) {
    if (wordCount(q)) {
      out = q;
      return true;
    }
  }
  return false;
}

bool prevTextPage(uint32_t from, uint32_t &out) {
  for (uint32_t q = from; q > 0; q--) {
    if (wordCount(q - 1)) {
      out = q - 1;
      return true;
    }
  }
  return false;
}

int gridCols() {
  return (bk->fontId() == BOOK_FONT_PROFONT29) ? TEXT_LARGE_COLS : TEXT_COLS;
}

// Did a paragraph end at the bottom of `page`? See the note on Word above.
// The end of the book and an image page in between both count as a break.
bool pageEndIsPara(uint32_t page) {
  const PageWords *pw = load(page);
  if (pw->endsBlank) return true;
  int lastCols = pw->lastLineCols;
  uint32_t next;
  if (!nextTextPage(page, next)) return true;
  if (next != page + 1) return true;
  return lastCols + 1 + load(next)->firstWordCols <= gridCols();
}

bool isParaEnd(const Pos &p) {
  const PageWords *pw = load(p.page);
  if (pw->words[p.word].paraEnd) return true;
  if ((size_t)p.word + 1 < pw->words.size()) return false;
  return pageEndIsPara(p.page);
}

// One word on; false at the end of the book (`p` is then unchanged).
bool advance(Pos &p) {
  if ((size_t)p.word + 1 < wordCount(p.page)) {
    p.word++;
    return true;
  }
  uint32_t next;
  if (!nextTextPage(p.page, next)) return false;
  p.page = next;
  p.word = 0;
  return true;
}

// One word back; false at the start of the book.
bool retreat(Pos &p) {
  if (p.word > 0) {
    p.word--;
    return true;
  }
  uint32_t prev;
  if (!prevTextPage(p.page, prev)) return false;
  p.page = prev;
  p.word = (uint16_t)(wordCount(prev) - 1);
  return true;
}

// Reading position follows the chunk on glass, saved the way a page turn
// saves it (main.cpp setPage: every change, no throttling).
void syncPage(uint32_t page) {
  if (page == curPage) return;
  curPage = page;
  books::savePosition(bk->slug(), curPage, bk->activeVariant());
  logf("speed read: page %lu/%lu\n", (unsigned long)(curPage + 1),
       (unsigned long)bk->pageCount());
}

uint32_t intervalMs() {
  return (uint32_t)wordsPerFlash * 60000UL / (uint32_t)wpmValue;
}

// Waveform cut for a flash held `hold` ms: none when the full waveform fits
// (RSVP_UNCUT_MIN_MS), otherwise the hold less the fixed per-flash overhead,
// floored at RSVP_CUT_MS. Always none on a driver without the cut hook
// (ui::speedHooksActive false, the stock GxEPD2 drivers).
// Also none on a panel whose row in src/panel.cpp has no cut (speedCutMs 0,
// the fast GDEY0579T93 driver, whose cut is a stub): there it would only arm
// the white push below on every other flash.
int cutFor(uint32_t hold) {
  if (!ui::speedHooksActive()) return 0;
  if (panel::activeSpec().speedCutMs == 0) return 0;
  if (hold >= RSVP_UNCUT_MIN_MS) return 0;
  const int budget = (int)hold - (int)RSVP_CUT_OVERHEAD_MS;
  return budget > RSVP_CUT_MS ? budget : RSVP_CUT_MS;
}

void saveWpm() {
  Preferences p;
  if (!p.begin(NVS_NS_UI, false)) return;
  p.putUShort(NVS_KEY_RSVP_WPM, (uint16_t)wpmValue);
  p.putUChar(NVS_KEY_RSVP_WORDS, (uint8_t)wordsPerFlash);
  p.end();
}

void saveMode() {
  Preferences p;
  if (!p.begin(NVS_NS_UI, false)) return;
  p.putUChar(NVS_KEY_RSVP_MODE, (uint8_t)ui::speedMode());
  p.end();
}

// Top speed for the current words per flash on the active panel: the active
// speed profile's wpmMax (src/panel.cpp, by speed mode) on a panel with
// profiles, else the panel's speedWpmMax row (panel 0's is
// RSVP_WPM_MAX_BY_WORDS). A row left at 0 falls back to panel 0's figure.
int wpmMax() {
  const speed::Profile *pr = ui::activeSpeedProfile();
  const int m = pr ? pr->wpmMax[wordsPerFlash]
                   : panel::activeSpec().speedWpmMax[wordsPerFlash];
  return m ? m : RSVP_WPM_MAX_BY_WORDS[wordsPerFlash];
}

// Ghost clearing cadence: a full at a sentence end after fullAfter partials,
// and unconditionally after fullMax. From the active speed profile when the
// panel has profiles, else the panel's speed fields (panel 0: RSVP_FULL_AFTER
// and RSVP_FULL_MAX).
int fullAfter() {
  const speed::Profile *pr = ui::activeSpeedProfile();
  return pr ? pr->fullAfter : panel::activeSpec().speedFullAfter;
}

int fullMax() {
  const speed::Profile *pr = ui::activeSpeedProfile();
  return pr ? pr->fullMax : panel::activeSpec().speedFullMax;
}

// Words per flash stepped by `dir` (+1 or -1) with wrap (3 -> 1, 1 -> 3), the
// speed clamped to the new mode's top speed, and both saved.
void stepWords(int dir) {
  wordsPerFlash = (wordsPerFlash - 1 + dir + 3) % 3 + 1;
  wpmValue = min(wpmValue, wpmMax());
  saveWpm();
}

// ----------------------------------------------------------------- drawing

int gfxTextWidth(Adafruit_GFX &g, const String &s) {
  int16_t x1, y1;
  uint16_t w, h;
  g.getTextBounds(s.c_str(), 0, 0, &x1, &y1, &w, &h);
  return x1 + w;
}

// `fonts` runs largest first; `base` is the working size. Leaves the chosen
// font set on `g` (and in `chosen`, when given) and returns the left edge.
int placeWords(Adafruit_GFX &g, const String &s, const GFXfont *const *fonts,
               int count, int base, const GFXfont **chosen = nullptr) {
  g.setTextWrap(false);
  // Base size unless the chunk would run off the right edge; then smaller
  // sizes, and as a last resort start it further left.
  int f = base, x = WORDS_LEFT;
  for (; f < count; f++) {
    g.setFont(fonts[f]);
    if (x + gfxTextWidth(g, s) <= g.width() - WORDS_EDGE) break;
  }
  if (f == count) {
    f = count - 1;
    g.setFont(fonts[f]);
    x = g.width() - WORDS_EDGE - gfxTextWidth(g, s);
    if (x < WORDS_EDGE) x = WORDS_EDGE;
  }
  if (chosen) *chosen = fonts[f];
  return x;
}

// `blit` draws the glyphs through ui::blitText when it can (same pixels, same
// cursor left behind; text size is 1 and placeWords turned wrap off, which
// is what blitText assumes), else through print() as always.
void drawWords(Adafruit_GFX &g, const String &s, int baseY,
               const GFXfont *const *fonts, int count, int base,
               bool blit = benchBlit) {
  g.setTextColor(ui::INK_BLACK);
  const GFXfont *font = nullptr;
  const int x = placeWords(g, s, fonts, count, base, &font);
  int endX, endY;
  blitUsed = blit && ui::blitText(font, x, baseY, s.c_str(), ui::INK_BLACK, endX, endY);
  if (blitUsed) {
    g.setCursor(endX, endY);
  } else {
    g.setCursor(x, baseY);
    g.print(s);
  }
  g.setFont(nullptr);
}

// Rows the chunk's glyph boxes cover when drawn as drawWords draws it, a
// superset of its ink. False for a chunk with no ink rows.
bool wordsRows(Adafruit_GFX &g, const String &s, int baseY,
               const GFXfont *const *fonts, int count, int base, int &y0,
               int &y1) {
  const int x = placeWords(g, s, fonts, count, base);
  int16_t bx, by;
  uint16_t bw, bh;
  g.getTextBounds(s.c_str(), x, baseY, &bx, &by, &bw, &bh);
  g.setFont(nullptr);
  if (!bw || !bh) return false;
  y0 = by;
  y1 = by + (int)bh - 1;
  return true;
}

// ------------------------------------------------------------------- HUD
//
// Header and footer are drawn as an instrument panel: tracked caps labels,
// condensed numeric readouts, a reticle on the reading band and a segmented
// progress bar. All of it is 1 bit and changes only on a page turn, a speed
// change or pause, so the flash-to-flash partial refresh never drives it.

const int HUD_L = RULE_INSET;
const int TRACK = 3;  // extra pixels between letters of HUD labels

int trackedWidth(const String &s, int track) {
  int w = 0;
  for (int i = 0; i < (int)s.length(); i++) w += ui::textWidth(String(s[i])) + track;
  return s.length() ? w - track : 0;
}

void printTracked(int x, int baseY, const String &s, int track = TRACK) {
  for (int i = 0; i < (int)s.length(); i++) {
    String c(s[i]);
    ui::printAt(x, baseY, c);
    x += ui::textWidth(c) + track;
  }
}

// Label over a big readout with a small unit after it. `labelY` and
// `valueY` are baselines (478 and 530 on the 680 px tall frame).
void readout(int x, int labelY, int valueY, const String &label,
             const String &value, const String &unit) {
  ui::setFont(ui::Font::HUD);
  printTracked(x, labelY, label);
  ui::setFont(ui::Font::HUD_NUM);
  ui::printAt(x, valueY, value);
  int vw = ui::textWidth(value);
  ui::setFont(ui::Font::HUD);
  printTracked(x + vw + 8, valueY, unit);
}

String upper(String s) {
  s.toUpperCase();
  return s;
}

// The panel line in the header: the part number (the last word of the panel's
// name in src/panel.cpp, skipping a trailing "800x480" style size) and the
// native frame. "GDEH0576T81  //  920X680" on panel 0.
String panelLabel() {
  const panel::Spec &sp = panel::activeSpec();
  String name = sp.name;
  name.trim();
  int cut = name.lastIndexOf(' ');
  String part = cut >= 0 ? name.substring(cut + 1) : name;
  String size = String(sp.nativeW) + "x" + String(sp.nativeH);
  if (part.equalsIgnoreCase(size) && cut > 0) {
    String rest = name.substring(0, cut);
    rest.trim();
    int cut2 = rest.lastIndexOf(' ');
    part = cut2 >= 0 ? rest.substring(cut2 + 1) : rest;
  }
  return upper(part) + "  //  " + String(sp.nativeW) + "X" + String(sp.nativeH);
}

String driveTag(const char *sep = "  /  ");  // below, with the HUD signature

void drawHeader(Adafruit_GFX &g, int w) {
  const int R = w - RULE_INSET;
  ui::setFont(ui::Font::HUD);
  g.fillRect(HUD_L, 39, 9, 9, ui::INK_BLACK);
  printTracked(HUD_L + 18, 48, "SPEED READ");
  String title = upper(bk->title());
  while (title.length() > 1 && trackedWidth(title, TRACK) > w / 2) {
    title.remove(title.length() - 1);
  }
  printTracked(R - trackedWidth(title, TRACK), 48, title);
  g.drawFastHLine(HUD_L, 62, R - HUD_L, ui::INK_BLACK);
  g.fillRect(HUD_L, 60, 110, 4, ui::INK_BLACK);
  g.fillRect(R - 24, 60, 24, 4, ui::INK_BLACK);
  // Telemetry: the real settings the panel is running. LUT and cut only
  // when the driver has those hooks; a stock driver runs its own LUT. A
  // panel with speed profiles shows the speed mode and LUT kind instead of
  // the forced LUT temperature.
  const String spi = "SPI " +
                     String((unsigned long)(panel::activeSpec().speedSpiHz / 1000000)) + "MHZ";
  String tele = "MODE " + String(wordsPerFlash) + "W   /   ALIGN L";
  const String drive = driveTag();
  if (drive.length()) {
    tele += "   /   " + drive + "   /   CUT AUTO " + String(cutFor(intervalMs())) + "MS   /   " +
            spi;
  } else if (ui::speedHooksActive()) {
    tele += "   /   LUT " + String(RSVP_LUT_TEMP) + "C   /   CUT AUTO " +
            String(cutFor(intervalMs())) + "MS   /   " + spi;
  } else {
    tele += "   /   STOCK LUT   /   " + spi;
  }
  printTracked(HUD_L, 90, tele, 2);
  String hw = panelLabel();
  printTracked(R - trackedWidth(hw, 2), 90, hw, 2);
}

// Scale ticks and end brackets on the band's rules, and pointers at the
// fixation point. The pointers reach `tri` px off the rules (20 on the tall
// layout, less on the short one) and are tri * 9 / 10 wide.
void drawReticle(Adafruit_GFX &g, int w, int top, int bot, int fx, int tri = 20) {
  const int tw = tri * 9 / 20;
  const int R = w - RULE_INSET;
  g.drawFastHLine(RULE_INSET, top, R - RULE_INSET, ui::INK_BLACK);
  g.drawFastHLine(RULE_INSET, bot, R - RULE_INSET, ui::INK_BLACK);
  for (int x = RULE_INSET; x <= R; x += 20) {
    if (abs(x - fx) < 16) continue;
    int len = ((x - RULE_INSET) % 100 == 0) ? 10 : 4;
    g.drawFastVLine(x, top - len, len, ui::INK_BLACK);
    g.drawFastVLine(x, bot + 1, len, ui::INK_BLACK);
  }
  // Corner brackets turning into the band.
  for (int x : {RULE_INSET, R}) {
    g.fillRect(x - 1, top, 3, 14, ui::INK_BLACK);
    g.fillRect(x - 1, bot - 13, 3, 14, ui::INK_BLACK);
  }
  g.fillTriangle(fx - tw, top - tri, fx + tw, top - tri, fx, top - 4, ui::INK_BLACK);
  g.fillTriangle(fx - tw, bot + tri, fx + tw, bot + tri, fx, bot + 4, ui::INK_BLACK);
  g.fillRect(fx - 1, top, 3, TICK_LEN, ui::INK_BLACK);
  g.fillRect(fx - 1, bot - TICK_LEN + 1, 3, TICK_LEN, ui::INK_BLACK);
}

// Minutes to the end of the book at the set rate. Words left come from the
// page anchors (characters to go over measured characters per word); an MPG1
// book has no anchors, so it falls back to pages to go at the average page.
double minutesLeftRaw() {
  const uint32_t pages = bk->pageCount();
  if (!pages || curPage >= pages) return 0;
  double wordsLeft;
  const uint32_t last = bk->anchorOf(pages - 1);
  if (last > 0) {
    // The last page's own length is unknown: count it as an average page.
    double end = last + (double)last / (pages - 1 ? pages - 1 : 1);
    wordsLeft = (end - bk->anchorOf(curPage)) * calWords / calChars;
  } else {
    wordsLeft = (double)(pages - curPage) * 250;
  }
  if (shown.page == curPage) wordsLeft -= shown.word;
  if (wordsLeft < 0) wordsLeft = 0;
  return wordsLeft / wpmValue * (calHeld / calNominal);
}

// What the footer shows. The raw estimate wobbles a minute or so from flash to
// flash (pacing and chars-per-word calibration update every flash and page),
// so the readout moves in steps of 5 minutes (1 under 10 minutes) and only
// once the estimate has drifted 3/4 of a step from what is on glass. A speed
// or words-per-flash change shows the new figure straight away.
int shownMins = -1;
int shownForWpm = 0;
int shownForWords = 0;

uint32_t minutesLeft() {
  const double raw = minutesLeftRaw();
  const int step = raw < 10 ? 1 : 5;
  const bool rateChanged = shownForWpm != wpmValue || shownForWords != wordsPerFlash;
  if (shownMins < 0 || rateChanged || fabs(raw - shownMins) > 0.75 * step) {
    shownMins = (int)(raw / step + 0.5) * step;
    shownForWpm = wpmValue;
    shownForWords = wordsPerFlash;
  }
  return (uint32_t)shownMins;
}

// Achieved speed, on the scale of the set one: the set wpm times the time the
// flashes were planned to hold (sentence and clause dwell included) over the
// real time between their starts, so a panel that keeps up reads the set
// speed. The same flash to flash intervals as benchStats().achievedWpm, but
// only the newest ones at the current settings (realFirst), and without the
// intervals that hold a ghost clearing full, which is planned overhead (the
// bench figure counts it). 0 = fewer than REAL_MIN_INTERVALS intervals.
int realMeasure() {
  if (flashNo < realFirst) return 0;
  // Settings changed since the last flash: nothing measured at them yet.
  if (wpmValue != realForWpm || wordsPerFlash != realForWords ||
      ui::speedMode() != realForMode) {
    return 0;
  }
  const int n = (int)min(flashNo - realFirst + 1, (uint32_t)statCount);
  const uint32_t RATE_GAP_MS = 5000;  // as benchStats: a longer gap is a pause
  uint32_t rateMs = 0, holdMs = 0;
  int used = 0;
  for (int k = 1; k < n; k++) {
    const FlashStat &a = stats[(statNext - n + k - 1 + STAT_SLOTS) % STAT_SLOTS];
    const FlashStat &c = stats[(statNext - n + k + STAT_SLOTS) % STAT_SLOTS];
    const uint32_t dt = c.at - a.at;
    if (c.afterFull || !dt || dt > RATE_GAP_MS) continue;
    rateMs += dt;
    holdMs += a.holdMs;
    used++;
  }
  if (used < REAL_MIN_INTERVALS || !rateMs) return 0;
  return (int)(((uint64_t)wpmValue * holdMs + rateMs / 2) / rateMs);
}

// What the HUD shows next to the rate: the achieved speed while playing and
// below REAL_SHOW_PCT of the set speed, in steps of 10 wpm, else 0 (nothing).
// Like minutesLeft() it moves only once the measure has drifted a step from
// what is on glass (20 wpm or 5% of the set speed), so it does not turn every
// flash into a whole screen compose; and it stays until the measure is back
// at REAL_HIDE_PCT. Every call between two flashes returns the same figure.
int realWpm() {
  if (!playing || ended) {
    realShown = 0;
    return 0;
  }
  const int r = realMeasure();
  const int limit = (realShown ? REAL_HIDE_PCT : REAL_SHOW_PCT) * wpmValue;
  if (!r || r * 100 >= limit) {
    realShown = 0;
    return 0;
  }
  const int q = (r + 5) / 10 * 10;
  const int step = max(20, wpmValue / 20);
  if (!realShown || abs(q - realShown) >= step) realShown = q;
  return realShown;
}

// Begins a new achieved speed measure from the next flash (resume, entry).
void realReset() {
  realFirst = flashNo + 1;
  realShown = 0;
}

// Speed mode and partial LUT kind for the telemetry, on a panel with speed
// profiles only ("" elsewhere): "TURBO  /  NOHOLD", or "TURBO NOHOLD" with
// sep " " (the short layout's crowded header). The LUT is the effective one,
// a bench console override included ("RAW" for a raw table).
String driveTag(const char *sep) {
  if (!ui::activeSpeedProfile()) return "";
  const ui::BenchDrive d = ui::benchDrive();
  const char *lut = d.lutRaw ? "RAW" : speed::lutKindName(d.lutKind);
  return upper(String(d.profile)) + sep + lut;
}

// The same as a number for the HUD signature: 0 without profiles.
uint8_t driveSig() {
  if (!ui::activeSpeedProfile()) return 0;
  const ui::BenchDrive d = ui::benchDrive();
  return (uint8_t)(0x80 | ((uint8_t)d.mode << 4) | (d.lutRaw ? 0x0F : (uint8_t)d.lutKind));
}

// Segmented progress bar with quarter marks at `by`, `bh` tall, and the
// PROGRESS label and percent 10 px above it. Both layouts.
void drawProgress(Adafruit_GFX &g, int w, int by, int bh) {
  const int R = w - RULE_INSET;
  const uint32_t pages = bk->pageCount();
  const int bw = R - HUD_L;
  g.drawRect(HUD_L, by, bw, bh, ui::INK_BLACK);
  int fillTo = HUD_L + 3 + (int)((uint64_t)(bw - 6) * (curPage + 1) / (pages ? pages : 1));
  for (int x = HUD_L + 3; x + 6 <= fillTo; x += 9) g.fillRect(x, by + 3, 6, bh - 6, ui::INK_BLACK);
  for (int q = 1; q < 4; q++) g.drawFastVLine(HUD_L + bw * q / 4, by + bh + 2, 6, ui::INK_BLACK);
  ui::setFont(ui::Font::HUD);
  String pct = String((unsigned long)((curPage + 1) * 100 / (pages ? pages : 1))) + "%";
  printTracked(R - trackedWidth(pct, TRACK), by - 10, pct);
  printTracked(HUD_L, by - 10, "PROGRESS");
}

// Status tag, inverted, at baseline `ty` (box ty - 17 to ty + 7). Returns
// the box's right edge.
int drawTag(Adafruit_GFX &g, int ty) {
  String tag = ended ? "END" : (playing ? "READING" : "PAUSED");
  const int tw = trackedWidth(tag, TRACK) + 38;
  g.fillRect(HUD_L, ty - 17, tw, 24, ui::INK_BLACK);
  ui::setInk(true);
  if (playing) {
    g.fillCircle(HUD_L + 13, ty - 5, 4, ui::INK_WHITE);
  } else {
    g.fillRect(HUD_L + 9, ty - 10, 3, 10, ui::INK_WHITE);
    g.fillRect(HUD_L + 15, ty - 10, 3, 10, ui::INK_WHITE);
  }
  printTracked(HUD_L + 26, ty, tag);
  ui::setInk(false);
  return HUD_L + tw;
}

// The controls for this state, in the build's button mapping (see handle()).
// Tier 0 is the full wording, the one the tall layout always shows; higher
// tiers are shorter, for the short layout's status line. nullptr past the
// last tier.
const char *hintFor(int tier) {
  const int state = ended ? 0 : (playing ? 1 : 2);
#if defined(PINS_WIDE) && PINS_WIDE
  static const char *const HINTS[3][4] = {
      {"CENTER  EXIT TO PAGE", "CENTER EXIT", nullptr, nullptr},
      {"UP DOWN  SPEED   /   RIGHT  WORDS   /   CENTER  PAUSE   /   2X CENTER  EXIT",
       "UP DN SPEED  /  RIGHT WORDS  /  CENTER PAUSE  /  2X EXIT",
       "UP DN SPEED / RIGHT WORDS / CENTER PAUSE",
       "UP DN SPEED / CENTER PAUSE"},
      {"CENTER  PLAY   /   UP DOWN  SPEED   /   RIGHT  WORDS   /   2X CENTER  EXIT",
       "CENTER PLAY  /  UP DN SPEED  /  RIGHT WORDS  /  2X EXIT",
       "CENTER PLAY / UP DN SPEED / RIGHT WORDS / 2X EXIT",
       "CENTER PLAY / 2X CENTER EXIT"},
  };
#else
  static const char *const HINTS[3][4] = {
      {"BACK  REWIND   /   CENTER  EXIT TO PAGE", "BACK REWIND  /  CENTER EXIT", nullptr,
       nullptr},
      {"FWD  +25   /   BACK  -25   /   CENTER  PAUSE", "FWD +25  /  BACK -25  /  CENTER PAUSE",
       nullptr, nullptr},
      {"FWD  PLAY   /   BACK  REWIND   /   CENTER  EXIT TO PAGE",
       "FWD PLAY  /  BACK REWIND  /  CENTER EXIT", nullptr, nullptr},
  };
#endif
  return (tier >= 0 && tier < 4) ? HINTS[state][tier] : nullptr;
}

// Everything below is anchored to the bottom edge `h` (680 on panel 0: the
// readout labels at h - 202 = 478, the bar at h - 112 = 568, the tag at
// h - 40 = 640), and the readout columns sit at the same fractions of the
// width as on the 920 px frame (330 and 620).
void drawFooter(Adafruit_GFX &g, int w, int h) {
  const int R = w - RULE_INSET;
  const uint32_t pages = bk->pageCount();
  const uint32_t mins = minutesLeft();
  char left[16];
  snprintf(left, sizeof(left), "%lu:%02lu", (unsigned long)(mins / 60), (unsigned long)(mins % 60));

  const int labelY = h - 202, valueY = h - 150;
  // The achieved speed, when the panel falls behind the set one, rides on
  // the RATE label.
  const int real = realWpm();
  String rateLabel = "RATE";
  if (real) rateLabel += "  /  REAL " + String(real);
  readout(HUD_L, labelY, valueY, rateLabel, String(wpmValue), "WPM");
  readout(w * 330 / 920, labelY, valueY, "PAGE",
          String((unsigned long)(curPage + 1)), "/ " + String((unsigned long)pages));
  readout(w * 620 / 920, labelY, valueY, "REMAINING", left, "H");

  drawProgress(g, w, h - 112, 14);
  drawTag(g, h - 40);
  String hint = hintFor(0);
  printTracked(R - trackedWidth(hint, 2), h - 40, hint, 2);
}

// ------------------------------------------------------------ short layout
//
// y budget on the 272 px bar (helvB10 caps ~10 px with ~3 px descenders;
// Gelasio34 caps ~45, tallest glyph 53 above the baseline, deepest 19 below;
// Gelasio28 44 / 16):
//     11- 23  header: square + SPEED READ, title, rate / page (baseline 20)
//     30- 42  top fixation pointer (tri 16, tip 4 px off the rule)
//     36- 45  scale ticks above the top rule
//         46  top rule; fixation tick 46-63
//     75-147  words, baseline 128 (34pt: 75-147; 28pt: 84-144)
//    153-170  bottom fixation tick; 170 bottom rule
//    171-186  scale ticks (to 180) and bottom pointer (174-186)
//    192-202  PROGRESS label and percent (baseline 202)
//    212-224  progress bar; quarter marks 226-232
//    238-262  status line: tag box, time left, controls hint (baseline 255)
// Nothing drawn below 262.
const int SH_HEAD_Y = 20;
const int SH_TOP = 46;
const int SH_BASE = 128;
const int SH_BOT = 170;
const int SH_TRI = 16;
const int SH_BAR_Y = 212;
const int SH_BAR_H = 12;
const int SH_STATUS_Y = 255;
const int SH_GAP = 24;  // between items sharing a line

void drawHeaderShort(Adafruit_GFX &g, int w) {
  const int R = w - RULE_INSET;
  const uint32_t pages = bk->pageCount();
  ui::setFont(ui::Font::HUD);
  g.fillRect(HUD_L, SH_HEAD_Y - 9, 9, 9, ui::INK_BLACK);
  const int labelX = HUD_L + 18;
  printTracked(labelX, SH_HEAD_Y, "SPEED READ");
  // Right: the settings that change while reading. The cut only when the
  // driver really cuts (ui::speedHooksActive) and the panel row has a cut;
  // the speed mode and LUT kind on a panel with speed profiles; the achieved
  // speed after the set one when the panel falls behind (realWpm).
  const int real = realWpm();
  String realPart;
  if (real) realPart = "REAL " + String(real) + "  /  ";
  String tele = String(wordsPerFlash) + "W  /  " + String(wpmValue) + " WPM  /  " + realPart +
                "P " + String((unsigned long)(curPage + 1)) + "/" + String((unsigned long)pages);
  const String drive = driveTag(" ");
  if (ui::speedHooksActive() && (!drive.length() || panel::activeSpec().speedCutMs)) {
    tele = "CUT " + String(cutFor(intervalMs())) + "MS  /  " + tele;
  }
  if (drive.length()) tele = drive + "  /  " + tele;
  const int teleX = R - trackedWidth(tele, 2);
  printTracked(teleX, SH_HEAD_Y, tele, 2);
  // Middle: the title, trimmed at the right to the gap between the two and
  // centred in it.
  const int gapL = labelX + trackedWidth("SPEED READ", TRACK) + SH_GAP;
  const int gapR = teleX - SH_GAP;
  if (gapR - gapL < 20) return;
  String title = upper(bk->title());
  while (title.length() > 1 && trackedWidth(title, TRACK) > gapR - gapL) {
    title.remove(title.length() - 1);
  }
  const int tw = trackedWidth(title, TRACK);
  if (tw <= gapR - gapL) printTracked(gapL + (gapR - gapL - tw) / 2, SH_HEAD_Y, title);
}

void drawFooterShort(Adafruit_GFX &g, int w) {
  const int R = w - RULE_INSET;
  drawProgress(g, w, SH_BAR_Y, SH_BAR_H);
  const int tagR = drawTag(g, SH_STATUS_Y);

  // Controls hint right; the widest wording that leaves room for the time
  // left after the tag, else the shortest one and no time left.
  const uint32_t mins = minutesLeft();
  char left[24];
  snprintf(left, sizeof(left), "%lu:%02lu H LEFT", (unsigned long)(mins / 60),
           (unsigned long)(mins % 60));
  const int timeX = tagR + SH_GAP - 8;
  const int timeR = timeX + trackedWidth(left, 2);
  String hint;
  bool showTime = false;
  for (int tier = 0; hintFor(tier); tier++) {
    hint = hintFor(tier);
    if (R - trackedWidth(hint, 2) >= timeR + SH_GAP) {
      showTime = true;
      break;
    }
  }
  if (showTime) printTracked(timeX, SH_STATUS_Y, left, 2);
  printTracked(R - trackedWidth(hint, 2), SH_STATUS_Y, hint, 2);
}

void compose() {
  Adafruit_GFX &g = ui::gfx();
  const int w = g.width();
  const int h = g.height();
  if (inkFrame) {  // soft clear, first half: the whole screen black
    g.fillScreen(ui::INK_BLACK);
    return;
  }
  g.fillScreen(ui::INK_WHITE);
  ui::setInk(false);
  if (isShort(g)) {
    drawHeaderShort(g, w);
    drawReticle(g, w, SH_TOP, SH_BOT, WORDS_LEFT + FIX_IN, SH_TRI);
    if (current.length()) {
      drawWords(g, current, SH_BASE, FONTS_SHORT, FONT_SHORT_COUNT, 0);
    }
    drawFooterShort(g, w);
    return;
  }
  const int baseY = h / 2 + 25;
  drawHeader(g, w);
  drawReticle(g, w, baseY - RULE_ABOVE, baseY + RULE_BELOW, WORDS_LEFT + FIX_IN);
  if (current.length()) drawWords(g, current, baseY, FONTS, FONT_COUNT, FONT_BASE);
  drawFooter(g, w, h);
}

// ------------------------------------------------------ incremental frames
//
// Flash to flash only the words change; the header, reticle and footer are a
// function of the HUD state below. GxEPD2_BW keeps its buffer across nextPage
// (the page is the whole panel; only firstPage clears it), so when the HUD
// state is what the buffer was last composed with, render() clears just the
// band interior (the rows strictly between the two rules, full width) and
// draws the reticle and the words again (BAND). The reticle is redrawn whole:
// outside the interior its pixels are already black in the buffer, so that is
// a no-op there, and inside it restores the brackets and fixation ticks.
//
// That equals a full compose bit for bit because, on both layouts, nothing
// but the reticle and the words draws on the interior rows (header and footer
// sit above and below the band, see the y budgets), nothing draws white over
// a reticle pixel, and the words never leave the interior: a chunk whose
// glyph boxes reach a rule or beyond (wordsRows) gets a full compose, and so
// does the frame after it, since its ink would linger outside the cleared rows.
//
// WINDOW sends only those interior rows to the panel (ui::frameBeginRawWindow)
// in a buffer composed from scratch in window coordinates, and leaves the rest
// of the glass as it is; the buffer is then window shaped, so the next whole
// screen frame is a full compose (the frame epoch moved). Not on a driver that
// ignores the window (panel 0), and not on a white push frame, which drives
// the whole screen. The GDEY0579T93 refresh runs the whole panel either way:
// a window saves transfer time only.
//
// Anything else (a full refresh, entry, a HUD change, another view having
// drawn, a rotation, `inc 0` and `win 0`) is a full compose, which is what
// every frame was before this.
struct HudSig {
  const Book *book;
  int w, h;
  int wpm, words;
  uint32_t page, pages;
  bool playing, ended;
  uint32_t mins;
  int cut;
  bool hooks;
  int real;       // realWpm(), 0 = not shown
  uint8_t drive;  // driveSig(): speed mode and LUT kind, 0 without profiles
  bool operator==(const HudSig &o) const {
    return book == o.book && w == o.w && h == o.h && wpm == o.wpm &&
           words == o.words && page == o.page && pages == o.pages &&
           playing == o.playing && ended == o.ended && mins == o.mins &&
           cut == o.cut && hooks == o.hooks && real == o.real && drive == o.drive;
  }
};

// The buffer's (and the glass's) last frame, as render() left it.
bool hudKnown = false;     // false: nothing to compare with, compose in full
HudSig hudLast = {};
bool hudSpill = false;     // its words reached outside the band interior
bool bufWhole = false;     // its buffer is a whole screen (not a window)
uint32_t bufEpoch = 0;     // ui::frameEpoch() right after it

// Everything the header and footer draw from. Every input of drawHeader,
// drawHeaderShort, drawFooter and drawFooterShort is here or fixed for the
// session (title, panel name, SPI clock); the hint tier and the percent
// follow from these. minutesLeft() and realWpm() are safe to call ahead of
// the header and footer: a second call right after the first returns the
// same figure.
HudSig hudNow(Adafruit_GFX &g) {
  HudSig s;
  s.book = bk;
  s.w = g.width();
  s.h = g.height();
  s.wpm = wpmValue;
  s.words = wordsPerFlash;
  s.page = curPage;
  s.pages = bk->pageCount();
  s.playing = playing;
  s.ended = ended;
  s.mins = minutesLeft();
  s.cut = cutFor(intervalMs());
  s.hooks = ui::speedHooksActive();
  s.real = realWpm();
  s.drive = driveSig();
  return s;
}

// The band of the layout: rule rows and words baseline.
void bandRows(Adafruit_GFX &g, int &top, int &bot, int &baseY) {
  if (isShort(g)) {
    top = SH_TOP;
    bot = SH_BOT;
    baseY = SH_BASE;
    return;
  }
  baseY = g.height() / 2 + 25;
  top = baseY - RULE_ABOVE;
  bot = baseY + RULE_BELOW;
}

// True when the chunk on glass reaches the rules or beyond.
bool wordsSpill(Adafruit_GFX &g, int top, int bot, int baseY) {
  if (!current.length()) return false;
  int y0, y1;
  const bool tall = !isShort(g);
  if (!wordsRows(g, current, baseY, tall ? FONTS : FONTS_SHORT,
                 tall ? FONT_COUNT : FONT_SHORT_COUNT, tall ? FONT_BASE : 0, y0, y1)) {
    return false;
  }
  return y0 <= top || y1 >= bot;
}

// The band interior, the reticle and the words, the same calls compose()
// makes. On a window frame the buffer is already white and the window drops
// everything outside it.
void composeBand(int top, int bot, int baseY) {
  Adafruit_GFX &g = ui::gfx();
  const int w = g.width();
  // Byte-level clear when the bench blit is on and the buffer allows it:
  // the per-pixel fillRect of the band was ~55 ms of every flash.
  if (!benchBlit || !ui::fillRowsWhite(top + 1, bot - top - 1)) {
    g.fillRect(0, top + 1, w, bot - top - 1, ui::INK_WHITE);
  }
  if (isShort(g)) {
    drawReticle(g, w, SH_TOP, SH_BOT, WORDS_LEFT + FIX_IN, SH_TRI);
    if (current.length()) {
      drawWords(g, current, SH_BASE, FONTS_SHORT, FONT_SHORT_COUNT, 0);
    }
    return;
  }
  drawReticle(g, w, top, bot, WORDS_LEFT + FIX_IN);
  if (current.length()) drawWords(g, current, baseY, FONTS, FONT_COUNT, FONT_BASE);
}

// The chunk on glass, drawn as compose() and composeBand() draw it.
void drawChunk(Adafruit_GFX &g, bool blit) {
  if (isShort(g)) {
    drawWords(g, current, SH_BASE, FONTS_SHORT, FONT_SHORT_COUNT, 0, blit);
  } else {
    drawWords(g, current, g.height() / 2 + 25, FONTS, FONT_COUNT, FONT_BASE, blit);
  }
}

// `allowWindow` false keeps the frame whole screen (a white push).
void render(bool full, bool allowWindow = true) {
  const uint32_t t0 = millis();
  lastFrameStartAt = t0;
  blitUsed = false;
  lastGapMs = lastFrameEndAt ? t0 - lastFrameEndAt : 0;
  Compose mode = Compose::FULL;
  const bool track = benchInc || benchWin;
  HudSig sig = {};
  bool spill = false;
  int top = 0, bot = 0, baseY = 0;
  uint32_t composeUs = 0;
  if (track) {
    const uint32_t c0 = micros();
    Adafruit_GFX &g = ui::gfx();
    sig = hudNow(g);
    bandRows(g, top, bot, baseY);
    spill = wordsSpill(g, top, bot, baseY);
    composeUs += micros() - c0;
    const bool same = !full && hudKnown && sig == hudLast && !hudSpill && !spill &&
                      ui::frameEpoch() == bufEpoch;
    if (same && benchWin && allowWindow && ui::windowFrames()) {
      mode = Compose::WINDOW;
    } else if (same && benchInc && bufWhole) {
      mode = Compose::BAND;
    }
  }
  if (mode == Compose::WINDOW) {
    ui::frameBeginRawWindow(top + 1, bot - top - 1);
  } else if (mode == Compose::BAND) {
    ui::frameBeginRawKeep();
  } else {
    ui::frameBeginRaw(full);
  }
  do {
    const uint32_t c0 = micros();
    if (mode == Compose::FULL) {
      compose();
    } else {
      composeBand(top, bot, baseY);
    }
    composeUs += micros() - c0;
  } while (ui::frameNext());
  ui::frameEnd();
  lastFrameEndAt = millis();
  lastFrameMs = lastFrameEndAt - t0;
  lastComposeMs = (composeUs + 500) / 1000;
  if (lastComposeMs > lastFrameMs) lastComposeMs = lastFrameMs;
  lastCompose = mode;
  hudKnown = track;
  if (track) {
    hudLast = sig;
    hudSpill = spill;
    bufWhole = mode != Compose::WINDOW;
    bufEpoch = ui::frameEpoch();
  }
  if (full) {
    lastRealFullAt = millis();
    partialsSinceFull = 0;
    cutsSincePush = 0;
    fullSinceFlash = true;
  } else {
    partialsSinceFull++;
  }
}

// Ends a sentence: . ? ! possibly followed by closing quotes or brackets.
bool endsSentence(const String &w) {
  int i = (int)w.length() - 1;
  while (i >= 0 && (w[i] == '"' || w[i] == '\'' || w[i] == ')' || w[i] == ']')) i--;
  return i >= 0 && (w[i] == '.' || w[i] == '?' || w[i] == '!');
}

bool endsClause(const String &w) {
  if (!w.length()) return false;
  char c = w[w.length() - 1];
  return c == ',' || c == ';' || c == ':' || c == '-';
}

// Builds the chunk starting at `start`, shows it, and moves `pos` past it.
// A chunk never runs past a sentence end, so every flash is a phrase that
// reads on its own. Returns true when the chunk ends a paragraph.
bool showChunk(const Pos &start, bool full) {
  String text;
  bool para = false;
  Pos p = start;
  havePos = false;
  int words = 0;
  String last;
  for (int i = 0; i < wordsPerFlash; i++) {
    const PageWords *pw = load(p.page);
    if (text.length()) text += ' ';
    words++;
    last = pw->words[p.word].text;
    text += last;
    bool pe = isParaEnd(p);
    havePos = advance(p);
    if (pe) {
      para = true;
      break;
    }
    if (!havePos || endsSentence(last)) break;
  }
  currentWords = words;
  // Hold sentence ends and clause breaks a little longer.
  currentDwell = (para || endsSentence(last)) ? RSVP_DWELL_SENTENCE
                 : endsClause(last)           ? RSVP_DWELL_CLAUSE
                                              : 1.0f;
  // Cut the waveform only when this chunk's hold is too short for the full
  // one, and then as late as the hold allows (see cutFor). Entry and
  // ghost-clearing frames are full refreshes, which the driver never cuts.
  // A white push (see RSVP_PUSH_AFTER) rides on the next flash once enough
  // cut flashes have run. Neither on a driver without the hooks (cutFor is
  // then 0 and there is nothing to push away).
  const int cut = cutFor((uint32_t)(intervalMs() * currentDwell));
  bool push = false;
  if (!full && ui::speedHooksActive()) {
    push = cutsSincePush >= RSVP_PUSH_AFTER || benchPushNext;
    benchPushNext = false;
    if (push) {
      cutsSincePush = 0;
    } else if (cut) {
      cutsSincePush++;
    }
  }
  ui::setSpeedCut(cut);
  ui::setSpeedPush(push);
  pos = p;
  shown = start;
  current = text;
  ended = false;
  syncPage(start.page);
  render(full, !push);
  if (full && softClearOn()) softClear();
  if (push) {
    ui::setSpeedPush(false);
    logf("speed read: white push\n");
  }
  return para;
}

// Feeds the rolling stats with the flash render() just drew, and with `log 1`
// prints it: flash number, frame ms, idle gap before it, start slip past due,
// the hold it had, cut, and the drive hooks. Late = the frame ran past its
// hold or started LATE_SLIP_MS past due.
void recordFlash(uint32_t hold, uint32_t slip) {
  flashNo++;
  // A speed, words or mode change starts a new achieved speed measure at
  // this flash, the first one planned with the new settings.
  if (wpmValue != realForWpm || wordsPerFlash != realForWords ||
      ui::speedMode() != realForMode) {
    realForWpm = wpmValue;
    realForWords = wordsPerFlash;
    realForMode = ui::speedMode();
    realFirst = flashNo;
  }
  const bool late = lastFrameMs > hold || slip > LATE_SLIP_MS;
  FlashStat &f = stats[statNext];
  f.at = lastFrameStartAt;
  f.afterFull = fullSinceFlash;
  fullSinceFlash = false;
  f.holdMs = (uint16_t)min(hold, (uint32_t)UINT16_MAX);
  f.words = (uint8_t)currentWords;
  f.blit = blitUsed;
  f.frameMs = (uint16_t)min(lastFrameMs, (uint32_t)UINT16_MAX);
  f.composeMs = (uint16_t)min(lastComposeMs, (uint32_t)UINT16_MAX);
  f.gapMs = (uint16_t)min(lastGapMs, (uint32_t)UINT16_MAX);
  f.late = late;
  f.compose = lastCompose;
  statNext = (statNext + 1) % STAT_SLOTS;
  if (statCount < STAT_SLOTS) statCount++;
  if (!benchLog) return;
  const ui::BenchDrive d = ui::benchDrive();
  // comp = inside compose, out = the rest of the frame (transfer + refresh);
  // then how the frame was composed (full, band, window) and whether its
  // words went through the glyph blit.
  const char *how = lastCompose == Compose::WINDOW ? "win"
                    : lastCompose == Compose::BAND ? "band"
                                                   : "full";
  logf("f %lu ms %lu gap %lu slip %lu hold %lu cut %d kp %d sw %d t %d comp %lu out %lu %s%s%s\n",
       (unsigned long)flashNo, (unsigned long)lastFrameMs, (unsigned long)lastGapMs,
       (unsigned long)slip, (unsigned long)hold, cutFor(hold), d.keepPower,
       d.singleWrite, d.temp, (unsigned long)lastComposeMs,
       (unsigned long)(lastFrameMs - lastComposeMs), how, blitUsed ? " blit" : "",
       late ? " LATE" : "");
}

// Speed drive (cut waveform, bench LUT) is for flashes only: a frame that
// stays on glass (paused, end, rewind) drawn with it is under-driven and fades
// to gray within minutes. Frames that hold run the stock OTP waveform.
bool fastDrive = false;

void driveFor(bool fast) {
  if (fast == fastDrive) return;
  fastDrive = fast;
  ui::setSpeedDrive(fast);
}

// Paused or ended frame: full refresh on the stock waveform, so every pixel
// (not just the changed ones) is driven all the way and the frame holds.
void renderHold() {
  driveFor(false);
  render(true);
}

void showEnd() {
  playing = false;
  ended = true;
  current = "End";
  renderHold();
  logf("speed read: end of book\n");
}

// Soft clear only where the drive hooks run a custom LUT (Turbo).
bool softClearOn() {
  return benchSoftClear && ui::speedHooksActive() && ui::activeSpeedProfile() &&
         ui::speedMode() == speed::Mode::TURBO;
}

// Two drive all partials: the whole screen black, then `current` (the blank
// band when it is empty). Run in place of the ghost clearing full, and right
// after every real full in Turbo, since a real full is what leaves the
// negatives. Counts as a ghost clear for the cadence and the rate stats.
void softClear() {
  ui::setSpeedPush(true);
  // hudKnown false: both frames compose whole (no band reuse).
  inkFrame = true;
  hudKnown = false;
  render(false, false);
  inkFrame = false;
  hudKnown = false;
  render(false, false);
  ui::setSpeedPush(false);
  partialsSinceFull = 0;
  cutsSincePush = 0;
  fullSinceFlash = true;
}
}  // namespace

namespace rsvp {

void begin() {
  Preferences p;
  if (!p.begin(NVS_NS_UI, true)) return;
  int v = p.getUShort(NVS_KEY_RSVP_WPM, RSVP_WPM_DEFAULT);
  int n = p.getUChar(NVS_KEY_RSVP_WORDS, RSVP_WORDS_DEFAULT);
  int m = p.getUChar(NVS_KEY_RSVP_MODE, (uint8_t)speed::Mode::NORMAL);
  p.end();
  // The mode first: the top speed below depends on it. Stored on every
  // panel (a panel without profiles ignores it), so a mode picked on the
  // 5.79 inch bar survives a trip to another panel.
  ui::setSpeedMode(m == (int)speed::Mode::TURBO ? speed::Mode::TURBO : speed::Mode::NORMAL);
  wordsPerFlash = constrain(n, 1, 3);
  wpmValue = constrain(v, RSVP_WPM_MIN, wpmMax());
}

speed::Mode mode() { return ui::speedMode(); }

void setMode(speed::Mode m) {
  // No profiles on this panel: nothing to switch.
  if (!ui::activeSpeedProfile()) return;
  if (m == ui::speedMode()) return;
  ui::setSpeedMode(m);  // re-applies the drive at once while it is on
  saveMode();
  // Clamp to the new mode's top speed; a console `wpm` keeps its own cap.
  const int was = wpmValue;
  wpmValue = constrain(wpmValue, RSVP_WPM_MIN, benchUncapped ? BENCH_WPM_MAX : wpmMax());
  if (wpmValue != was && !benchUncapped) saveWpm();
  logf("speed read: %s mode, %d wpm\n", m == speed::Mode::TURBO ? "turbo" : "normal",
       wpmValue);
  // Paused or ended in speed read: show the new settings now (the drive is
  // off then, as after benchSetWpm). While playing the next flash does.
  if (active && !playing) render(false);
}

void cycleMode() {
  if (!ui::activeSpeedProfile()) return;
  setMode(ui::speedMode() == speed::Mode::TURBO ? speed::Mode::NORMAL : speed::Mode::TURBO);
}

int wpm() { return wpmValue; }

int words() { return wordsPerFlash; }

void cycleWords() { stepWords(1); }

void cycleWpm() {
  wpmValue += RSVP_WPM_STEP;
  if (wpmValue > wpmMax()) wpmValue = RSVP_WPM_MIN;
  saveWpm();
}

uint16_t resumeWord(const String &slug, uint8_t variant, uint32_t page) {
  if (slug == lastSlug && variant == lastVariant && page == lastPage) return lastWord;
  return 0;
}

void enter(Book &book, uint32_t page, uint16_t word) {
  bk = &book;
  clearCache();
  shownMins = -1;
  curPage = page;
  playing = true;
  ended = false;
  fullPending = false;
  active = true;
  benchPushNext = false;
  hudKnown = false;
  lastFrameEndAt = 0;
  statCount = statNext = 0;
  flashNo = 0;
  fullSinceFlash = false;
  realReset();
  // The stored speed may be over this panel's top speed (a speed set on
  // another panel); begin() clamped against the panel active at boot. A
  // console `wpm` is exempt.
  wpmValue = constrain(wpmValue, RSVP_WPM_MIN, benchUncapped ? BENCH_WPM_MAX : wpmMax());

  ui::setLandscape(true);
  fastDrive = false;
  driveFor(true);
  logf("speed read: %s from page %lu word %u, %d wpm\n", book.slug().c_str(),
       (unsigned long)(page + 1), (unsigned)word, wpmValue);

  Pos start;
  bool found = false;
  if (page < book.pageCount() && wordCount(page)) {
    start.page = page;
    start.word = (word < wordCount(page)) ? word : 0;
    found = true;
  } else if (page < book.pageCount()) {
    found = nextTextPage(page, start.page);
    start.word = 0;
  }
  // Full refresh on entry: the panel is coming from a portrait frame.
  if (found) {
    showChunk(start, true);
  } else {
    // Nothing left to read from here: straight to the end state.
    shown.page = page;
    shown.word = 0;
    havePos = false;
    playing = false;
    ended = true;
    current = "End";
    render(true);
  }
  logf("speed read: full refresh (entry)\n");
  nextDue = millis() + intervalMs();
}

bool tick() {
  if (!playing) return false;
  uint32_t now = millis();
  if (nextDue && (int32_t)(now - nextDue) < 0) return false;
  driveFor(true);  // back from a pause: flashes run fast again
  const uint32_t interval = intervalMs();

  if (fullPending) {
    // The sentence's last chunk has had its full interval; clear the ghosts
    // on a blank band, then give the next chunk half an interval.
    fullPending = false;
    current = "";
    if (softClearOn() && millis() - lastRealFullAt < RSVP_REAL_FULL_EVERY_MS) {
      softClear();
      logf("speed read: soft clear (page %lu)\n", (unsigned long)(curPage + 1));
    } else {
      render(true);
      if (softClearOn()) softClear();
      logf("speed read: full refresh (paragraph, page %lu)\n",
           (unsigned long)(curPage + 1));
    }
    nextDue = millis() + interval / 2;
    return true;
  }
  if (!havePos) {
    showEnd();
    return true;
  }

  uint32_t t0 = millis();
  const uint32_t slip = nextDue ? t0 - nextDue : 0;
  bool para = showChunk(pos, false);
  uint32_t took = millis() - t0;
  // The chunk now on glass sets how long it stays; the schedule below runs
  // one interval behind, so scale the gap to the next flash by its dwell.
  const uint32_t hold = (uint32_t)(interval * currentDwell);
  recordFlash(hold, slip);
  // Pacing calibration, in seconds: held vs. nominal for the words shown.
  calHeld += hold / 1000.0;
  calNominal += currentWords * 60.0 / wpmValue;

  // Next due one interval after the last one, or after this frame when the
  // schedule has slipped by more than an interval: late frames never pile up
  // into a burst.
  now = millis();
  if (nextDue && (int32_t)(now - (nextDue + hold)) < 0) {
    nextDue += hold;
  } else {
    nextDue = now + (took > hold ? 0 : hold - took);
  }
  if ((int32_t)(nextDue - now) < 0) nextDue = now;

  // Ghost clearing cadence is per panel and speed mode (see fullAfter).
  const bool sentenceEnd = para || currentDwell >= RSVP_DWELL_SENTENCE;
  if ((sentenceEnd && partialsSinceFull >= fullAfter()) ||
      partialsSinceFull >= fullMax()) {
    fullPending = true;
  }
  return true;
}

bool handle(Button b) {
#if defined(PINS_WIDE) && PINS_WIDE
  // Wide reader: LEFT does nothing here (on the founder's v4 build the
  // module's L line is not read at all, pins.h PAD_LEFT_OFF). UP and DOWN
  // step the speed, RIGHT cycles the words per flash (1, 2, 3), CENTER plays
  // or pauses, and a quick second CENTER tap exits to the page. Keep hintFor()
  // in step.
  static uint32_t lastCenterMs = 0;
  switch (b) {
    case BTN_UP:
    case BTN_DOWN: {
#if defined(FOUNDER_UNIT) && FOUNDER_UNIT
      // DOWN speeds up: on the v4 build the PAD_FLIP turn leaves UP and DOWN
      // the wrong way round for speed (founder, 2026-10-31).
      const Button faster = BTN_DOWN;
#else
      const Button faster = BTN_UP;
#endif
      wpmValue = b == faster ? min(wpmValue + RSVP_WPM_STEP, wpmMax())
                             : max(wpmValue - RSVP_WPM_STEP, RSVP_WPM_MIN);
      saveWpm();
      logf("speed read: %d wpm\n", wpmValue);
      if (!playing) {
        driveFor(false);
        render(false);
      }
      return false;
    }
    case BTN_RIGHT:
      // Takes effect from the next chunk; while playing the next flash redraws
      // the header, while paused or at the end redraw it now.
      stepWords(1);
      logf("speed read: %d words, %d wpm\n", wordsPerFlash, wpmValue);
      if (!playing) {
        driveFor(false);
        render(false);
      }
      return false;
    case BTN_CENTER: {
      const uint32_t now = millis();
      const bool second = lastCenterMs && now - lastCenterMs < RSVP_EXIT_TAP_MS;
      lastCenterMs = now;
      if (second || ended) {
        lastCenterMs = 0;
        return true;
      }
      if (playing) {
        playing = false;
        renderHold();
      } else {
        playing = true;
        nextDue = 0;  // next chunk straight away
        realReset();
      }
      return false;
    }
    default:
      return false;
  }
#else
  switch (b) {
    // On the cased 5.76 inch unit only GPIO 1 (BTN_LEFT, the founder's
    // "forward"), GPIO 41 (BTN_DOWN, "back") and CENTER work (see
    // include/pins.h); RIGHT and UP are their twins.
    case BTN_LEFT:
    case BTN_RIGHT:
      if (playing) {
        wpmValue = min(wpmValue + RSVP_WPM_STEP, wpmMax());
        saveWpm();
        logf("speed read: %d wpm\n", wpmValue);
      } else if (!ended) {
        playing = true;
        nextDue = 0;  // next chunk straight away
        realReset();
        // The paused frame drew the hint; the next chunk clears it.
      }
      return false;
    case BTN_DOWN:
    case BTN_UP:
      if (playing) {
        wpmValue = max(wpmValue - RSVP_WPM_STEP, RSVP_WPM_MIN);
        saveWpm();
        logf("speed read: %d wpm\n", wpmValue);
      } else {
        Pos p = shown;
        for (int i = 0; i < RSVP_REWIND_CHUNKS * wordsPerFlash; i++) {
          if (!retreat(p)) break;
        }
        if (wordCount(p.page)) {
          driveFor(false);
          showChunk(p, true);
        }
      }
      return false;
    case BTN_CENTER:
      if (playing) {
        playing = false;
        renderHold();
        return false;
      }
      return true;
    default:
      return false;
  }
#endif
}

void leave() {
  if (bk) {
    lastSlug = bk->slug();
    lastVariant = bk->activeVariant();
    lastPage = curPage;
    lastWord = (shown.page == curPage) ? shown.word : 0;
  }
  playing = false;
  active = false;
  hudKnown = false;
  clearCache();
  driveFor(false);
  ui::setLandscape(false);
  logf("speed read: left on page %lu\n", (unsigned long)(curPage + 1));
}

uint32_t page() { return curPage; }

// ------------------------------------------------------------ bench console

bool benchActive() { return active; }

bool benchPlaying() { return playing; }

void benchSetWpm(int v) {
  wpmValue = constrain(v, RSVP_WPM_MIN, BENCH_WPM_MAX);
  benchUncapped = true;
  if (active && !playing) render(false);
}

void benchSetWords(int n) {
  wordsPerFlash = constrain(n, 1, 3);
  if (active && !playing) render(false);
}

void benchForceFull() { fullPending = true; }

void benchPushOnce() { benchPushNext = true; }
void benchSetSoftClear(bool on) { benchSoftClear = on; }

void benchSetLog(bool on) { benchLog = on; }

void benchSetIncremental(bool on) { benchInc = on; }

void benchSetWindow(bool on) { benchWin = on; }

void benchSetBlit(bool on) { benchBlit = on; }

BlitCheck benchBlitCheck() {
  BlitCheck r = {};
  if (!active) {
    r.why = "not in speed read";
    return r;
  }
  if (!current.length()) {
    r.why = "no words on glass";
    return r;
  }
  uint8_t *buf = nullptr;
  size_t n = 0;
  if (!ui::frameBytes(buf, n)) {
    r.why = "no blit here (buffer not addressable or portrait rotation)";
    return r;
  }
  // The live buffer is put back at the end, so the next incremental frame
  // still draws over exactly what is on glass.
  uint8_t *keep = (uint8_t *)malloc(n);
  uint8_t *ref = (uint8_t *)malloc(n);
  if (!keep || !ref) {
    free(keep);
    free(ref);
    r.why = "out of memory";
    return r;
  }
  memcpy(keep, buf, n);
  Adafruit_GFX &g = ui::gfx();
  const uint8_t rot = g.getRotation();
  const bool blitWas = blitUsed;
  r.bytes = n;
  r.ran = true;
  // Both ways at the current rotation and at the one 180 degrees from it
  // (Flip screen), each over a white and over a striped background, so a
  // stray white write shows as well as a stray black one.
  for (int pass = 0; pass < 4; pass++) {
    const bool flipped = pass >= 2;
    g.setRotation(flipped ? rot ^ 2 : rot);
    const uint8_t bg = (pass & 1) ? 0x5A : 0xFF;
    memset(buf, bg, n);
    uint32_t t = micros();
    drawChunk(g, false);
    r.gfxUs = micros() - t;
    memcpy(ref, buf, n);
    memset(buf, bg, n);
    t = micros();
    drawChunk(g, true);
    r.blitUs = micros() - t;
    if (!blitUsed) r.fellBack++;
    int diff = 0;
    for (size_t i = 0; i < n; i++) diff += buf[i] != ref[i];
    if (flipped) {
      r.mismatchFlip += diff;
    } else {
      r.mismatch += diff;
    }
  }
  g.setRotation(rot);
  memcpy(buf, keep, n);
  free(keep);
  free(ref);
  blitUsed = blitWas;
  return r;
}

bool benchPlay() {
  if (!active || ended) return false;
  if (!playing) {
    playing = true;
    nextDue = 0;  // next chunk straight away
    realReset();
  }
  return true;
}

bool benchPause() {
  if (!active) return false;
  if (playing) {
    playing = false;
    renderHold();  // like the button pause: drive off (rails down), frame held on the stock waveform
  }
  return true;
}

void benchResetStats() {
  statCount = statNext = 0;
}

BenchStats benchStats() {
  BenchStats b = {};
  b.wpm = wpmValue;
  b.words = wordsPerFlash;
  b.intervalMs = intervalMs();
  b.cutMs = cutFor(b.intervalMs);
  b.flashes = flashNo;
  b.partialsSinceFull = partialsSinceFull;
  b.fullPending = fullPending;
  b.pushPending = benchPushNext;
  b.log = benchLog;
  b.incremental = benchInc;
  b.window = benchWin;
  b.windowCapable = ui::windowFrames();
  b.blit = benchBlit;
  b.blitCapable = ui::blitAvailable();
  b.count = statCount;
  if (!statCount) return b;
  uint32_t sum = 0, gapSum = 0, composeSum = 0;
  b.minMs = UINT32_MAX;
  for (int i = 0; i < statCount; i++) {
    const FlashStat &f = stats[i];
    sum += f.frameMs;
    composeSum += f.composeMs;
    gapSum += f.gapMs;
    if (f.frameMs < b.minMs) b.minMs = f.frameMs;
    if (f.frameMs > b.maxMs) b.maxMs = f.frameMs;
    if (f.late) b.late++;
    if (f.compose == Compose::BAND) b.bandFlashes++;
    if (f.compose == Compose::WINDOW) b.windowFlashes++;
    if (f.blit) b.blitFlashes++;
  }
  // Rates over the flashes in order, oldest first: a chunk's words count
  // over the time to the next flash's start (actual) or over its hold
  // (planned); the newest has no successor yet. Gaps over RATE_GAP_MS (a
  // pause) are skipped. Ghost clearing fulls between two flashes count as
  // the time they took.
  const uint32_t RATE_GAP_MS = 5000;
  uint32_t rateWords = 0, rateMs = 0, holdMs = 0;
  for (int k = 1; k < statCount; k++) {
    const FlashStat &a = stats[(statNext - statCount + k - 1 + STAT_SLOTS) % STAT_SLOTS];
    const FlashStat &c = stats[(statNext - statCount + k + STAT_SLOTS) % STAT_SLOTS];
    const uint32_t dt = c.at - a.at;
    if (!dt || dt > RATE_GAP_MS) continue;
    rateWords += a.words;
    rateMs += dt;
    holdMs += a.holdMs;
  }
  if (rateMs) b.achievedWpm = (int)((rateWords * 60000UL + rateMs / 2) / rateMs);
  if (holdMs) b.plannedWpm = (int)((rateWords * 60000UL + holdMs / 2) / holdMs);
  b.avgMs = sum / statCount;
  b.avgComposeMs = composeSum / statCount;
  b.avgOutMs = b.avgMs - b.avgComposeMs;
  b.avgGapMs = gapSum / statCount;
  return b;
}

}  // namespace rsvp
