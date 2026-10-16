#include "gbgfx.h"

#include "config.h"
#include "pack.h"
#include "pokemon_state.h"
#include "ui.h"

namespace {

const char *SECTION_FONT = "font";
const char *SECTION_BORDER = "border";
const size_t FONT_BYTES = 256 * 8;   // 256 tiles, 1 bpp, 8 bytes each
const size_t BORDER_BYTES = 9 * 8;

uint8_t *fontPage = nullptr;
bool fontTried = false;
uint8_t borderTiles[BORDER_BYTES];
bool borderTried = false;
bool borderOk = false;

// Layout map: panel = origin + floor(design * num / den). num == den and a
// zero origin is the identity (panel 0).
int layNum = 1, layDen = 1;
int layOx = 0, layOy = 0;
int layH = 0;

// Floor division that stays right for a negative design coordinate.
int scaleFloor(int v) {
  long n = (long)v * layNum;
  long q = n / layDen;
  if ((n % layDen) != 0 && n < 0) q--;
  return (int)q;
}

// The panel rectangle a design rectangle covers, clamped to at least 1 px.
void mapRect(int x, int y, int w, int h, int &px, int &py, int &pw, int &ph) {
  px = layOx + scaleFloor(x);
  py = layOy + scaleFloor(y);
  pw = layOx + scaleFloor(x + w) - px;
  ph = layOy + scaleFloor(y + h) - py;
  if (w > 0 && pw < 1) pw = 1;
  if (h > 0 && ph < 1) ph = 1;
}

void loadFont() {
  if (fontTried) return;
  fontTried = true;
  if (!pack::isOpen() || pack::size(SECTION_FONT) < FONT_BYTES) return;
  fontPage = (uint8_t *)malloc(FONT_BYTES);
  if (!fontPage) return;
  if (pack::read(SECTION_FONT, 0, fontPage, FONT_BYTES) != FONT_BYTES) {
    free(fontPage);
    fontPage = nullptr;
  }
}

void loadBorder() {
  if (borderTried) return;
  borderTried = true;
  if (!pack::isOpen() || pack::size(SECTION_BORDER) < BORDER_BYTES) return;
  borderOk = pack::read(SECTION_BORDER, 0, borderTiles, BORDER_BYTES) ==
             BORDER_BYTES;
}

// One shade of one source pixel, expanded to a scale x scale block. Shade 1 is
// one dot per 2x2 cell and shade 2 a checker, both keyed off the absolute
// panel pixel position so neighbouring tiles line up into one continuous
// pattern (on panel 0 a panel pixel is a design pixel).
void blockForShade(int px, int py, int scale, uint8_t shade, bool dither) {
  if (shade == 0) return;
  if (shade == 3) {
    gbgfx::fillRect(px, py, scale, scale, ui::INK_BLACK);
    return;
  }
  if (scale < 2 || !dither) {
    // No room for a pattern: threshold, shade 2 goes black and shade 1 white.
    if (shade == 2) gbgfx::fillRect(px, py, scale, scale, ui::INK_BLACK);
    return;
  }
  Adafruit_GFX &g = ui::gfx();
  int x0, y0, w, h;
  mapRect(px, py, scale, scale, x0, y0, w, h);
  for (int ay = y0; ay < y0 + h; ay++) {
    for (int ax = x0; ax < x0 + w; ax++) {
      bool on = (shade == 2) ? (((ax + ay) & 1) == 0)
                             : (((ax & 1) == 0) && ((ay & 1) == 0));
      if (on) g.drawPixel(ax, ay, ui::INK_BLACK);
    }
  }
}

}  // namespace

namespace gbgfx {

void setLayout(int designW, int designH) {
  if (designW < 1 || designH < 1) return;
  // The ratio is the smaller of the two fits, as a fraction, never above 1.
  // Cross multiplied: SCREEN_W/designW < SCREEN_H/designH.
  if ((long)SCREEN_W * designH < (long)SCREEN_H * designW) {
    layNum = SCREEN_W;
    layDen = designW;
  } else {
    layNum = SCREEN_H;
    layDen = designH;
  }
  if (layNum >= layDen) layNum = layDen = 1;
  layOx = (SCREEN_W - scaleFloor(designW)) / 2;
  layOy = 0;
  layH = scaleFloor(designH);
}

int mapX(int x) { return layOx + scaleFloor(x); }
int mapY(int y) { return layOy + scaleFloor(y); }
int layoutHeight() { return layH; }

void fillRect(int x, int y, int w, int h, uint16_t color) {
  int px, py, pw, ph;
  mapRect(x, y, w, h, px, py, pw, ph);
  if (pw > 0 && ph > 0) ui::gfx().fillRect(px, py, pw, ph, color);
}

void drawPixel(int x, int y, uint16_t color) { fillRect(x, y, 1, 1, color); }

void fillSparse(int x, int y, int w, int h) {
  int px, py, pw, ph;
  mapRect(x, y, w, h, px, py, pw, ph);
  if (pw <= 0 || ph <= 0) return;
  Adafruit_GFX &g = ui::gfx();
  g.fillRect(px, py, pw, ph, ui::INK_WHITE);
  for (int ay = py; ay < py + ph; ay++) {
    for (int ax = px; ax < px + pw; ax++) {
      if (((ax & 1) == 0) && ((ay & 1) == 0)) g.drawPixel(ax, ay, ui::INK_BLACK);
    }
  }
}

void setOriginY(int y) { layOy = y; }

void reset() {
  if (fontPage) {
    free(fontPage);
    fontPage = nullptr;
  }
  fontTried = false;
  borderTried = false;
  borderOk = false;
}

bool fontReady() {
  loadFont();
  return fontPage != nullptr;
}

void drawTile2bpp(const uint8_t *tile16, int x, int y, int scale, bool dither) {
  if (!tile16 || scale < 1) return;
  for (int row = 0; row < 8; row++) {
    uint8_t lo = tile16[row * 2];
    uint8_t hi = tile16[row * 2 + 1];
    // Runs of black are one fillRect instead of eight; the dithered shades
    // still go pixel by pixel.
    int col = 0;
    while (col < 8) {
      uint8_t bit = (uint8_t)(7 - col);
      uint8_t shade = (uint8_t)(((lo >> bit) & 1) | (((hi >> bit) & 1) << 1));
      bool solid = (shade == 3) || (shade == 2 && (scale < 2 || !dither));
      if (solid) {
        int run = 1;
        while (col + run < 8) {
          uint8_t b2 = (uint8_t)(7 - (col + run));
          uint8_t s2 = (uint8_t)(((lo >> b2) & 1) | (((hi >> b2) & 1) << 1));
          bool solid2 = (s2 == 3) || (s2 == 2 && (scale < 2 || !dither));
          if (!solid2) break;
          run++;
        }
        gbgfx::fillRect(x + col * scale, y + row * scale, run * scale, scale,
                        ui::INK_BLACK);
        col += run;
      } else {
        blockForShade(x + col * scale, y + row * scale, scale, shade, dither);
        col++;
      }
    }
  }
}

void drawTile1bpp(const uint8_t *tile8, int x, int y, int scale, bool inverted) {
  if (!tile8 || scale < 1) return;
  if (inverted) gbgfx::fillRect(x, y, 8 * scale, 8 * scale, ui::INK_BLACK);
  uint16_t ink = inverted ? ui::INK_WHITE : ui::INK_BLACK;
  for (int row = 0; row < 8; row++) {
    uint8_t bits = tile8[row];
    if (!bits) continue;   // a clear row is paper, already painted
    int col = 0;
    while (col < 8) {
      if (!((bits >> (7 - col)) & 1)) { col++; continue; }
      int run = 1;
      while (col + run < 8 && ((bits >> (7 - col - run)) & 1)) run++;
      gbgfx::fillRect(x + col * scale, y + row * scale, run * scale, scale, ink);
      col += run;
    }
  }
}

void drawFontCode(uint8_t code, int x, int y, int scale, bool inverted) {
  loadFont();
  if (!fontPage) return;
  drawTile1bpp(fontPage + (size_t)code * 8, x, y, scale, inverted);
}

int lineHeightGb(int scale) { return 8 * (scale < 1 ? 1 : scale); }

int textWidthGb(const String &s, int scale) {
  if (scale < 1) scale = 1;
  return (int)s.length() * 8 * scale;
}

int printGb(int x, int y, const char *ascii, int scale, bool inverted) {
  if (!ascii) return x;
  if (scale < 1) scale = 1;
  loadFont();
  if (!fontPage) {
    // No pack: fall back to the reader's own text layer. `y` is the top of the
    // cell here, and ui::printAt wants a baseline. The 7x14 font is not
    // scaled, so only its position goes through the layout map; the x it
    // returns is converted back to design space (exact on panel 0).
    ui::setFont(ui::Font::STATUS);
    ui::setInk(inverted);
    ui::printAt(mapX(x), mapY(y + 8 * scale - 2), String(ascii));
    ui::setInk(false);
    int w = ui::textWidth(String(ascii));
    return x + (int)((long)w * layDen / layNum);
  }
  int cx = x;
  for (const char *p = ascii; *p; p++) {
    uint8_t code = pokemon::gen1Encode(*p);
    drawTile1bpp(fontPage + (size_t)code * 8, cx, y, scale, inverted);
    cx += 8 * scale;
  }
  return cx;
}

int printGb(int x, int y, const String &s, int scale, bool inverted) {
  return printGb(x, y, s.c_str(), scale, inverted);
}

void drawBox(int x, int y, int wTiles, int hTiles, int scale) {
  if (wTiles < 2 || hTiles < 2) return;
  if (scale < 1) scale = 1;
  loadBorder();
  int step = 8 * scale;
  if (!borderOk) {
    // Two 1 px frames, drawn as design space edges so they shrink with the
    // page (drawRect on panel 0, pixel for pixel).
    const int W = wTiles * step, H = hTiles * step;
    fillRect(x, y, W, H, ui::INK_WHITE);
    for (int k = 0; k <= 2; k += 2) {
      fillRect(x + k, y + k, W - 2 * k, 1, ui::INK_BLACK);
      fillRect(x + k, y + H - 1 - k, W - 2 * k, 1, ui::INK_BLACK);
      fillRect(x + k, y + k, 1, H - 2 * k, ui::INK_BLACK);
      fillRect(x + W - 1 - k, y + k, 1, H - 2 * k, ui::INK_BLACK);
    }
    return;
  }
  for (int ty = 0; ty < hTiles; ty++) {
    int rowKind = (ty == 0) ? 0 : (ty == hTiles - 1 ? 2 : 1);
    for (int tx = 0; tx < wTiles; tx++) {
      int colKind = (tx == 0) ? 0 : (tx == wTiles - 1 ? 2 : 1);
      const uint8_t *tile = borderTiles + (size_t)(rowKind * 3 + colKind) * 8;
      drawTile1bpp(tile, x + tx * step, y + ty * step, scale, false);
    }
  }
}

}  // namespace gbgfx
