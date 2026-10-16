#include "rsvp.h"

#include <Preferences.h>

#include <vector>

#include "Gelasio28pt7b.h"
#include "Gelasio34pt7b.h"
#include "Gelasio40pt7b.h"
#include "Gelasio48pt7b.h"
#include "config.h"
#include "panel.h"
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
// Landscape, the panel's native frame: 920x680 on the 5.76 inch panel, the
// only one speed read is enabled on (panel::speedReadSupported). The words
// band is centred on the height and the footer is anchored to the bottom
// edge, but the header, band and footer together need the full 680 px: on
// a 480 px tall landscape frame (the 7.5 inch panel) the footer readouts
// would land on the band, so a new panel needs its own pass here before its
// row in src/panel.cpp turns speed read on. Two hairline rules across the width frame the band the
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
bool fullPending = false;    // clear ghosts at the next due time
uint32_t nextDue = 0;

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

void saveWpm() {
  Preferences p;
  if (!p.begin(NVS_NS_UI, false)) return;
  p.putUShort(NVS_KEY_RSVP_WPM, (uint16_t)wpmValue);
  p.putUChar(NVS_KEY_RSVP_WORDS, (uint8_t)wordsPerFlash);
  p.end();
}

int wpmMax() { return RSVP_WPM_MAX_BY_WORDS[wordsPerFlash]; }

// ----------------------------------------------------------------- drawing

int gfxTextWidth(Adafruit_GFX &g, const String &s) {
  int16_t x1, y1;
  uint16_t w, h;
  g.getTextBounds(s.c_str(), 0, 0, &x1, &y1, &w, &h);
  return x1 + w;
}

void drawWords(Adafruit_GFX &g, const String &s, int baseY) {
  g.setTextColor(ui::INK_BLACK);
  g.setTextWrap(false);
  // Base size unless the chunk would run off the right edge; then smaller
  // sizes, and as a last resort start it further left.
  int f = FONT_BASE, x = WORDS_LEFT;
  for (; f < FONT_COUNT; f++) {
    g.setFont(FONTS[f]);
    if (x + gfxTextWidth(g, s) <= g.width() - WORDS_EDGE) break;
  }
  if (f == FONT_COUNT) {
    f = FONT_COUNT - 1;
    g.setFont(FONTS[f]);
    x = g.width() - WORDS_EDGE - gfxTextWidth(g, s);
    if (x < WORDS_EDGE) x = WORDS_EDGE;
  }
  g.setCursor(x, baseY);
  g.print(s);
  g.setFont(nullptr);
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
  // Telemetry: the real settings the panel is running.
  String tele = "MODE " + String(wordsPerFlash) + "W   /   ALIGN L" +
                "   /   LUT " + String(RSVP_LUT_TEMP) + "C   /   CUT AUTO " +
                String(RSVP_CUT_MS) + "MS   /   SPI " +
                String((unsigned long)(RSVP_SPI_HZ / 1000000)) + "MHZ";
  printTracked(HUD_L, 90, tele, 2);
  String hw = panelLabel();
  printTracked(R - trackedWidth(hw, 2), 90, hw, 2);
}

// Scale ticks and end brackets on the band's rules, and pointers at the
// fixation point.
void drawReticle(Adafruit_GFX &g, int w, int top, int bot, int fx) {
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
  g.fillTriangle(fx - 9, top - 20, fx + 9, top - 20, fx, top - 4, ui::INK_BLACK);
  g.fillTriangle(fx - 9, bot + 20, fx + 9, bot + 20, fx, bot + 4, ui::INK_BLACK);
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
  readout(HUD_L, labelY, valueY, "RATE", String(wpmValue), "WPM");
  readout(w * 330 / 920, labelY, valueY, "PAGE",
          String((unsigned long)(curPage + 1)), "/ " + String((unsigned long)pages));
  readout(w * 620 / 920, labelY, valueY, "REMAINING", left, "H");

  // Segmented progress bar with quarter marks.
  const int by = h - 112, bh = 14, bw = R - HUD_L;
  g.drawRect(HUD_L, by, bw, bh, ui::INK_BLACK);
  int fillTo = HUD_L + 3 + (int)((uint64_t)(bw - 6) * (curPage + 1) / (pages ? pages : 1));
  for (int x = HUD_L + 3; x + 6 <= fillTo; x += 9) g.fillRect(x, by + 3, 6, bh - 6, ui::INK_BLACK);
  for (int q = 1; q < 4; q++) g.drawFastVLine(HUD_L + bw * q / 4, by + bh + 2, 6, ui::INK_BLACK);
  ui::setFont(ui::Font::HUD);
  String pct = String((unsigned long)((curPage + 1) * 100 / (pages ? pages : 1))) + "%";
  printTracked(R - trackedWidth(pct, TRACK), by - 10, pct);
  printTracked(HUD_L, by - 10, "PROGRESS");

  // Status tag, inverted, and the controls for this state.
  String tag = ended ? "END" : (playing ? "READING" : "PAUSED");
  const int ty = h - 40, tw = trackedWidth(tag, TRACK) + 38;
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
  String hint = ended     ? "BACK  REWIND   /   CENTER  EXIT TO PAGE"
                : playing ? "FWD  +25   /   BACK  -25   /   CENTER  PAUSE"
                          : "FWD  PLAY   /   BACK  REWIND   /   CENTER  EXIT TO PAGE";
  printTracked(R - trackedWidth(hint, 2), ty, hint, 2);
}

void compose() {
  Adafruit_GFX &g = ui::gfx();
  const int w = g.width();
  const int h = g.height();
  const int baseY = h / 2 + 25;
  g.fillScreen(ui::INK_WHITE);
  ui::setInk(false);
  drawHeader(g, w);
  drawReticle(g, w, baseY - RULE_ABOVE, baseY + RULE_BELOW, WORDS_LEFT + FIX_IN);
  if (current.length()) drawWords(g, current, baseY);
  drawFooter(g, w, h);
}

void render(bool full) {
  ui::frameBeginRaw(full);
  do {
    compose();
  } while (ui::frameNext());
  ui::frameEnd();
  if (full) {
    partialsSinceFull = 0;
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
  // one (see RSVP_UNCUT_MIN_MS). Entry and ghost-clearing frames are full
  // refreshes, which the driver never cuts.
  const uint32_t hold = (uint32_t)(intervalMs() * currentDwell);
  ui::setSpeedCut(hold >= RSVP_UNCUT_MIN_MS ? 0 : RSVP_CUT_MS);
  pos = p;
  shown = start;
  current = text;
  ended = false;
  syncPage(start.page);
  render(full);
  return para;
}

void showEnd() {
  playing = false;
  ended = true;
  current = "End";
  render(false);
  logf("speed read: end of book\n");
}

}  // namespace

namespace rsvp {

void begin() {
  Preferences p;
  if (!p.begin(NVS_NS_UI, true)) return;
  int v = p.getUShort(NVS_KEY_RSVP_WPM, RSVP_WPM_DEFAULT);
  int n = p.getUChar(NVS_KEY_RSVP_WORDS, RSVP_WORDS_DEFAULT);
  p.end();
  wordsPerFlash = constrain(n, 1, 3);
  wpmValue = constrain(v, RSVP_WPM_MIN, wpmMax());
}

int wpm() { return wpmValue; }

int words() { return wordsPerFlash; }

void cycleWords() {
  wordsPerFlash = wordsPerFlash % 3 + 1;
  wpmValue = min(wpmValue, wpmMax());
  saveWpm();
}

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

  ui::setLandscape(true);
  ui::setSpeedDrive(true);
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
  const uint32_t interval = intervalMs();

  if (fullPending) {
    // The sentence's last chunk has had its full interval; clear the ghosts
    // on a blank band, then give the next chunk half an interval.
    fullPending = false;
    current = "";
    render(true);
    logf("speed read: full refresh (paragraph, page %lu)\n",
         (unsigned long)(curPage + 1));
    nextDue = millis() + interval / 2;
    return true;
  }
  if (!havePos) {
    showEnd();
    return true;
  }

  uint32_t t0 = millis();
  bool para = showChunk(pos, false);
  uint32_t took = millis() - t0;
  // The chunk now on glass sets how long it stays; the schedule below runs
  // one interval behind, so scale the gap to the next flash by its dwell.
  const uint32_t hold = (uint32_t)(interval * currentDwell);
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

  const bool sentenceEnd = para || currentDwell >= RSVP_DWELL_SENTENCE;
  if ((sentenceEnd && partialsSinceFull >= RSVP_FULL_AFTER) ||
      partialsSinceFull >= RSVP_FULL_MAX) {
    fullPending = true;
  }
  return true;
}

bool handle(Button b) {
  switch (b) {
    // Only GPIO 1 (BTN_LEFT, the founder's "forward"), GPIO 41 (BTN_DOWN,
    // "back") and CENTER work on this unit; RIGHT and UP are their twins.
    case BTN_LEFT:
    case BTN_RIGHT:
      if (playing) {
        wpmValue = min(wpmValue + RSVP_WPM_STEP, wpmMax());
        saveWpm();
        logf("speed read: %d wpm\n", wpmValue);
      } else if (!ended) {
        playing = true;
        nextDue = 0;  // next chunk straight away
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
        if (wordCount(p.page)) showChunk(p, false);
      }
      return false;
    case BTN_CENTER:
      if (playing) {
        playing = false;
        render(false);
        return false;
      }
      return true;
    default:
      return false;
  }
}

void leave() {
  if (bk) {
    lastSlug = bk->slug();
    lastVariant = bk->activeVariant();
    lastPage = curPage;
    lastWord = (shown.page == curPage) ? shown.word : 0;
  }
  playing = false;
  clearCache();
  ui::setSpeedDrive(false);
  ui::setLandscape(false);
  logf("speed read: left on page %lu\n", (unsigned long)(curPage + 1));
}

uint32_t page() { return curPage; }

}  // namespace rsvp
