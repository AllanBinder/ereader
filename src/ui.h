#pragma once

#include <Adafruit_GFX.h>
#include <Arduino.h>
#include <U8g2_for_Adafruit_GFX.h>

#include <vector>

#include "config.h"

namespace ui {

// Brings up the panel stored in NVS (src/panel.h, missing = panel 0): reads
// the choice, fills the geometry globals and creates its driver. Call once,
// early in setup(), before WiFi or anything else that fragments the heap: the
// frame buffer is one contiguous block of up to ~78 KB.
void begin();

// Tears down the live driver (frame buffer first) and brings up panel `id` in
// its place, geometry included, without a reboot and without touching NVS.
// The boot-time picker uses it to cycle through the panels; pair it with
// panel::set() to keep a choice. Rotation goes back to the new panel's
// DISPLAY_ROTATION. False when the driver could not be allocated, in which
// case it falls back to panel 0.
bool beginPanel(uint8_t id);

// Panel rotation, 1 or 3 (both portrait, 180 degrees apart). begin() starts at
// DISPLAY_ROTATION; main.cpp applies the stored "Flip screen" choice. The next
// frame after a change should be a full refresh.
void setRotation(uint8_t rotation);
uint8_t rotation();

// Speed read (src/rsvp.cpp) runs the panel landscape and faster, and puts
// both back when it is done, so the rest of the app never sees either.
//
// setLandscape(true) maps the portrait rotation to the landscape one the same
// way up (1 -> 0, 3 -> 2), giving the native frame as the canvas (920x680 on
// panel 0); false restores the portrait rotation it came from. That holds for
// every panel in src/panel.cpp, whose native frames are all landscape.
// setSpeedDrive(true) runs SPI at RSVP_SPI_HZ and forces the partial LUT
// temperature to RSVP_LUT_TEMP; false puts back the panel's own SPI clock and
// the internal sensor. Follow either with a full refresh. The LUT and cut
// hooks exist only on panels with panel::speedReadSupported(); elsewhere
// setSpeedDrive and setSpeedCut leave the panel alone.
void setLandscape(bool on);
void setSpeedDrive(bool on);
// Waveform cut for the next speed read flashes, in ms (0 = full waveform).
void setSpeedCut(int ms);

// ------------------------------------------------------------ card geometry
//
// Shared by the menu (renderMenu below) and the daily numbers view
// (src/numbers_view.cpp), so the two read as one family: the same header
// band, the same two column grid of hairline cards, the same gutter and pad.
// A tile is (SCREEN_W - 2*16 - 12) / 2 wide: (648 - 12) / 2 = 318 px on panel
// 0, 218 on panel 1. The row count and therefore the tile height is each
// view's own. GRID_BOTTOM and TILE_W follow the active panel, so they are
// globals filled by panel::applyGeometry(), not constants.
static const int HEADER_H = 44;             // solid black band at the top
static const int HEADER_BASELINE_Y = 29;    // baseline of 7x14 text in it
static const int GRID_COLS = 2;
static const int GRID_GUTTER = 12;
static const int GRID_BOTTOM_INSET = 16;    // grid ends this far above the rule
extern int GRID_BOTTOM;  // STATUS_RULE_Y - GRID_BOTTOM_INSET: 880 | 760
extern int TILE_W;       // 318 | 218
static const int TILE_PAD = 14;             // inset of the tile content
static const int TILE_ACCENT_W = 36;        // short 2 px hairline under a label

// ------------------------------------------------------- drawing primitives
//
// The panel and the u8g2 text layer live in ui.cpp. A view that owns its own
// layout (numbers_view) composes a frame with these instead of reaching in.

enum class Font : uint8_t {
  SMALL,        // 6x12, status strip left
  STATUS,       // 7x14, status strip right, tile sub lines
  STATUS_BOLD,  // 7x14 bold
  BODY,         // profont22, page text
  LABEL,        // helvB24, bold proportional
  NUM,          // fub42, digits and , . + - only (no letters)
  CAPTION,      // helvB14, bold proportional, speed read status line
  HUD,          // helvB10, speed read HUD labels (tracked caps)
  HUD_NUM,      // logisoso38, speed read HUD readouts
};
void setFont(Font f);
int textWidth(const String &s);
void setInk(bool inverted);  // white on black when inverted
void printAt(int x, int baselineY, const String &s);
// Cuts `s` to `maxW` pixels in the current font, ending in ".." if anything
// was dropped.
String fitText(const String &s, int maxW);
// The rule and the three status strip fields: `left` small, `right` 7x14 (or
// bold), the clock between them when both leave room.
void drawStatusStrip(const String &left, const String &right,
                     bool boldRight = false);
// The panel as a GFX canvas, for rectangles and lines, and the two colours
// it knows (the same values as GxEPD_BLACK / GxEPD_WHITE, without pulling the
// panel driver's headers into a view).
Adafruit_GFX &gfx();
static const uint16_t INK_BLACK = 0x0000;
static const uint16_t INK_WHITE = 0xFFFF;

// A frame is: frameBegin(); do { draw } while (frameNext()); frameEnd().
// frameBegin picks the waveform on the same policy as every renderer here
// (partial unless forced, unless the panel still shows an image page, unless
// the ration says it is time for a full), frameEnd records that a text frame
// is now on glass.
void frameBegin(bool forceFull);
bool frameNext();
void frameEnd();

// Same frame, but the caller owns the refresh policy: `full` picks the
// waveform outright and the ration above is neither consulted nor advanced.
// The speed reader flashes hundreds of partials a minute and times its own
// ghost clearing to paragraph breaks.
void frameBeginRaw(bool full);

// Reading view: page text is rendered verbatim, one screen line per '\n'.
// `fontId` picks the text grid (BOOK_FONT_* in config.h) and must be the font
// id of the variant the page was paginated for.
void renderPage(const String &title, const String &pageText, uint8_t fontId,
                uint32_t pageIndex, uint32_t pageCount, bool forceFull = false);

// Reading view for an image page. `bits` is the stored bitmap: rows top to
// bottom, MSB first inside each byte, bit 1 = black, stride ceil(width/8).
// flags bit0 (MPG1_IMAGE_FLAG_DOUBLE) draws every stored pixel as a 2x2 block.
// Image pages always take a full refresh, and so does the text page that
// follows one, because dithered art ghosts badly under partial refresh.
void renderImagePage(const String &title, const uint8_t *bits, uint16_t width,
                     uint16_t height, uint8_t flags, uint32_t pageIndex,
                     uint32_t pageCount);

// One tile of the grid menu. `label` is the bold line (book title or action
// name), `sub` the small state line under it ("p. 123/352", "Large", "off"),
// empty for none. Both are cut to the tile width with ".." on the glass.
struct MenuTile {
  String label;
  String sub;
  MenuTile() {}
  MenuTile(const String &l, const String &s) : label(l), sub(s) {}
};

// Menu view: a two column grid of tiles under a header band carrying the
// sensor line. `cursor` indexes `tiles` row major (0 and 1 are the first
// row). The tile at the cursor is drawn inverted. Rows scroll to keep the
// cursor's row on screen when there are more tiles than fit.
void renderMenu(const String &header, const std::vector<MenuTile> &tiles,
                int cursor, bool forceFull = false);

// Grid shape, so the navigation code in main.cpp agrees with the renderer.
int menuColumns();

// Jump to page view: the target number is shown large.
void renderJump(const String &title, uint32_t target, uint32_t pageCount,
                bool forceFull = false);

// Full screen message (empty library, missing book, parse failure). Always a
// full refresh: these screens stay up for a long time.
void renderMessage(const String &title, const std::vector<String> &lines);

// Same layout as renderMessage, but on the ordinary partial refresh policy.
// This is what the WiFi setup and news sync progress screens use: they redraw
// every few seconds and must not spend a full flash each time. The usual
// ration still applies, so a full refresh lands every PARTIALS_BEFORE_FULL
// updates and clears the ghosting.
void renderStatusScreen(const String &title, const std::vector<String> &lines,
                        bool forceFull = false);

}  // namespace ui
