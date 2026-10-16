#pragma once

#include <Arduino.h>

// Game Boy tile and Gen 1 font drawing on the 1 bit panel.
//
// Everything the companion screen draws comes from the pack (src/pack.*):
// 2 bpp graphics tiles, a 1 bpp font page in Gen 1 charmap order, and the nine
// text box border tiles. The four GB shades are mapped to a 2x2 dither at
// scale 2 and up (0 white, 1 one dot in four, 2 checker, 3 black) and to a
// threshold at scale 1, where a dither would just look like noise.
//
// Drawing goes through ui::gfx(), so these calls belong inside a
// ui::frameBegin() / frameNext() paging loop like any other view.
//
// Layout space. The companion views are laid out to a pixel contract on a
// 680x920 page (docs/pokemon-aux-display-layout.md). Every coordinate and
// size handed to this module is in that design space, and setLayout() maps it
// onto the active canvas: scaled by the largest ratio that fits the design on
// the canvas (never up), centred horizontally, top aligned. Each design pixel
// covers [map(p), map(p + 1)) on the panel, so neighbouring tiles butt up
// without gaps and the dither patterns are drawn on panel pixels, not design
// pixels. On panel 0 (680x920) the ratio is 1 and the map is the identity, so
// the output is the same pixels as before the layer existed. On the 7.5 inch
// panel (480x800) the ratio is 480/680, about 0.71: the page is 480x649, a
// scale 2 Gen 1 cell is about 11 px (2.3 mm at that panel's 124 dpi, against
// 2.0 mm on panel 0), and strokes come out 1 or 2 px wide.

namespace gbgfx {

// Fits a designW x designH page onto the active canvas (SCREEN_W/SCREEN_H at
// the time of the call). Call before drawing a frame.
void setLayout(int designW, int designH);
// Design x / y to a panel x / y.
int mapX(int x);
int mapY(int y);
// Height in panel pixels of the scaled design page.
int layoutHeight();
// A design space rectangle in panel pixels. A rectangle at least one design
// pixel across is never less than one panel pixel across, so hairlines
// survive the shrink.
void fillRect(int x, int y, int w, int h, uint16_t color);
void drawPixel(int x, int y, uint16_t color);
// A design rectangle painted white with one black dot per 2x2 panel pixels
// (the GB's lightest shade), keyed off panel coordinates.
void fillSparse(int x, int y, int w, int h);
// Moves the design page's top edge to panel row `y` (0 after setLayout). The
// companion views use it to pin their hint band to the bottom of a canvas
// taller than the scaled page.
void setOriginY(int y);

// One 8x8 tile in GB planar 2 bpp: 16 bytes, two bytes per row, low bitplane
// first. Drawn at `scale` (1 = 8x8 px, 2 = 16x16 px, and so on).
void drawTile2bpp(const uint8_t *tile16, int x, int y, int scale, bool dither);

// One 8x8 tile in 1 bpp: 8 bytes, one per row, MSB is the leftmost pixel,
// bit 1 = ink. `inverted` swaps ink and paper (white glyph on a black cell).
void drawTile1bpp(const uint8_t *tile8, int x, int y, int scale, bool inverted);

// Gen 1 font text. Each character is mapped to its Gen 1 code and drawn from
// the pack's "font" section (256 tiles of 8 bytes, charmap order). The section
// is cached in RAM on first use. Falls back to ui::printAt with the reader's
// own font when the pack has no font section, so a screen without a pack still
// says something. Returns the x past the last cell drawn.
int printGb(int x, int y, const char *ascii, int scale, bool inverted);
int printGb(int x, int y, const String &s, int scale, bool inverted);

// One glyph of the pack's font page by its raw Gen 1 character code, for the
// handful of tiles that have no ASCII spelling (the accented e of POKeDEX at
// $BA, the box rules at $79 to $7E). Draws nothing when there is no pack.
void drawFontCode(uint8_t code, int x, int y, int scale, bool inverted);

// Width in pixels printGb would take for `s` at `scale`.
int textWidthGb(const String &s, int scale);

// Height of one printGb line in pixels.
int lineHeightGb(int scale);

// The in-game text box: a wTiles x hTiles frame of border tiles from the
// pack's "border" section (9 tiles in the order top-left, top, top-right,
// left, center, right, bottom-left, bottom, bottom-right). The interior is
// filled with the centre tile. Draws a plain rectangle when the pack has no
// border section.
void drawBox(int x, int y, int wTiles, int hTiles, int scale);

// Drops the cached font page. Call when the pack is closed.
void reset();

// True when the pack carries a usable font page, so a view can decide between
// the game look and the reader's own font.
bool fontReady();

}  // namespace gbgfx
