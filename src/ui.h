#pragma once

#include <Adafruit_GFX.h>
#include <Arduino.h>
#include <U8g2_for_Adafruit_GFX.h>

#include <vector>

#include "config.h"
#include "speed_profile.h"

namespace ui {

// Brings up the panel stored in NVS (src/panel.h, missing = the build's
// DEFAULT_PANEL_ID): reads
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

// Panel rotation: DISPLAY_ROTATION or the one 180 degrees from it, so 1 or 3
// on a portrait UI and 0 or 2 on a landscape one (panel 2). Anything else
// falls back to DISPLAY_ROTATION. begin() starts at DISPLAY_ROTATION;
// main.cpp applies the stored "Flip screen" choice. The next frame after a
// change should be a full refresh.
void setRotation(uint8_t rotation);
uint8_t rotation();
// The other rotation of the pair above, 180 degrees from `rotation`.
uint8_t flippedRotation(uint8_t rotation);
// True when `rotation` is one of the pair above for the active panel.
bool validRotation(uint8_t rotation);

// Speed read (src/rsvp.cpp) runs the panel landscape and faster, and puts
// both back when it is done, so the rest of the app never sees either.
//
// setLandscape(true) maps the portrait rotation to the landscape one the same
// way up (1 -> 0, 3 -> 2), giving the native frame as the canvas (920x680 on
// panel 0); false restores the portrait rotation it came from. That holds for
// every panel in src/panel.cpp, whose native frames are all landscape. On a
// panel whose UI already runs on the native frame (an even DISPLAY_ROTATION,
// panel 2) both are the identity: the canvas is landscape already.
// setSpeedDrive(true) runs SPI at the active panel's speedSpiHz (panel 0:
// RSVP_SPI_HZ). On a panel without speed profiles it forces the partial LUT
// temperature to RSVP_LUT_TEMP; on a panel with them (panel 2) it applies the
// active profile (activeSpeedProfile: forced temperature, keep power, single
// write, lean timing, LUT built for the panel temperature sampled once here,
// skip previous), then any bench override on top. false puts back the
// panel's own SPI clock, the internal sensor, the OTP waveform and turns
// every speed hook off. Follow either with a full refresh. On panels without panel::speedReadSupported()
// setSpeedDrive, setSpeedCut and setSpeedPush leave the panel alone; on a
// supported panel whose driver has no LUT, cut or push hooks (stock GxEPD2,
// see speedHooksActive) only the SPI clock changes.
void setLandscape(bool on);
void setSpeedDrive(bool on);
// True when the active panel supports speed read and its driver really has
// the LUT temperature, cut and push hooks (Display::hasSpeedHooks). False
// means cut and push requests do nothing, so speed read should not plan them.
bool speedHooksActive();
// Speed mode (Normal / Turbo). Stored here, not persisted (rsvp.cpp owns the
// NVS setting and calls this). Changing it while the drive is on re-applies
// the profile at once (temperature sampled again). On a panel without
// profiles it is stored and has no effect.
void setSpeedMode(speed::Mode m);
speed::Mode speedMode();
// The active panel's profile for the current mode, or nullptr when the panel
// has no profiles (panels 0 and 1). A panel with a Normal but no Turbo
// profile answers Normal in both modes.
const speed::Profile *activeSpeedProfile();
// Waveform cut for the next speed read flashes, in ms (0 = full waveform).
void setSpeedCut(int ms);
// Drive every pixel on the next speed read partials (a white push), not only
// the changed ones. Off again after the frame; see rsvp.cpp.
void setSpeedPush(bool on);

// Bench console hooks (src/bench.cpp, the fast panel 2 driver's tuning
// session). Each is stored as an override that wins over the speed profile
// until benchClearOverrides(), and applied at once while speed read's drive
// is on, otherwise by the next setSpeedDrive(true); setSpeedDrive(false)
// always turns keep power and single write off on the panel (the stored
// choice stays). Drivers without the hook ignore it (see panel::Display).
// Keep power: analog rails stay up between partials.
void benchSetKeepPower(bool on);
// Single write: skip the redundant 0x24 half of each partial frame write.
void benchSetSingleWrite(bool on);
// Forced partial LUT temperature in degrees C, -1 = the internal sensor.
// Until set, speed read forces the profile's forcedTemp, or RSVP_LUT_TEMP on
// a panel without profiles, as before.
void benchSetTemp(int t);
// Partial LUT override, nullptr = OTP. Not copied: must stay valid while set.
void benchSetCustomLut(const uint8_t *lut, size_t len);
// Partial LUT override built here with speed::buildLut (kind OTP = the OTP
// waveform). Frames are used as given, without the temperature steps. Lets
// benchDrive report the kind; prefer it over benchSetCustomLut.
void benchSetLut(speed::LutKind kind, int frames, int frameRate, bool pol);
void benchSetSourceLevels(uint8_t vsh1, uint8_t vsh2, uint8_t vsl);
void benchSetSkipPrevious(bool on);
// Speed read SPI clock override (0 = the panel spec's speedSpiHz).
void benchSetSpiHz(uint32_t hz);
// Bulk SPI frame writes (Display::setBulkSpi). Unlike the hooks above it is
// the driver's own setting, on by default in the fast panel 2 driver, and
// applies at once to every frame, in speed read or not. A new driver
// (beginPanel) starts at its default again.
void benchSetBulkSpi(bool on);
// Lean timing and profile lines (Display::setLeanTiming / setProfileLog), the
// driver's own settings like bulk SPI.
void benchSetLeanTiming(bool on);
void benchSetProfileLog(bool on);
// Drops every override above except bulk SPI and profile lines (the driver's
// own settings): the profile (or, on a panel without one, the fixed drive)
// is back in charge, re-applied at once while the drive is on.
void benchClearOverrides();
struct BenchDrive {
  bool speedDrive;      // speed read's drive is on
  bool bulkSpi;         // the driver's bulk SPI state (false: none or off)
  bool leanTiming;      // the driver's lean timing state (false: none or off)
  bool keepPower;       // effective: the console's choice, else the profile's
  bool singleWrite;
  bool tempOverridden;  // false: `temp` is the profile's, or RSVP_LUT_TEMP without one
  int temp;             // forced OTP temperature during speed read, -1 = sensor
  speed::Mode mode;       // the speed mode set
  bool hasProfile;        // the active panel has speed profiles
  const char *profile;    // active profile name, "" without one
  speed::LutKind lutKind; // effective partial LUT kind (OTP when unknown)
  bool lutRaw;            // the LUT is a benchSetCustomLut table (kind unknown)
  int frames;             // effective custom LUT frames, 0 for OTP or raw
  int frameRate;          // FR code of the effective custom LUT, 0 for OTP or raw
  int tempC;              // panel temperature at the last drive on (INT16_MIN unknown)
  bool skipPrevious;      // effective skip previous during speed read
  bool leanSet;           // effective lean timing during speed read
  // Which settings the console has overridden (they win over the profile).
  bool ovKeepPower, ovSingleWrite, ovLean, ovSkipPrevious, ovLut, ovTemp;
};
BenchDrive benchDrive();

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
static const int GRID_TOP_GAP = 20;         // menu grid starts this far below the band
static const int GRID_BOTTOM_INSET = 16;    // grid ends this far above the rule
static const int CARD_MIN_H = 126;          // the full size menu card's content
extern int GRID_BOTTOM;  // STATUS_RULE_Y - GRID_BOTTOM_INSET: 880 | 760 | 232
extern int TILE_W;       // 318 | 218 | 374

// Short canvas: a content area too short for two rows of full size cards
// under the full band, GRID_BOTTOM - (HEADER_H + GRID_TOP_GAP) <
// 2*CARD_MIN_H + GRID_GUTTER. Panel 0 has 816 px for the 264 that needs and
// panel 1 has 696; the 5.79 inch bar (panel 2, 792x272) has 232 - 64 = 168,
// so it is short. A short canvas gets the thin band below and the compact
// forms of the menu and numbers views; every other view derives its layout
// from SCREEN_W and SCREEN_H directly. Filled by panel::applyGeometry().
extern bool SHORT_CANVAS;
// The thin band: 7x14 text (and helvB14 for a title) in 28 px instead of the
// 44 px band with helvB24. 19 centres the 7x14 caps (10 px) in the band.
static const int HEADER_SHORT_H = 28;
static const int HEADER_SHORT_BASELINE_Y = 19;
// The band of the active canvas: HEADER_H / HEADER_BASELINE_Y, or the thin
// band on a short canvas.
int headerH();
int headerBaselineY();
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
// Greedy word wrap of `s` into at most `maxLines` lines of `maxW` pixels in
// the current font; the last line takes the rest and is cut with "..".
std::vector<String> wrapText(const String &s, int maxW, int maxLines);
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

// Fast GFXfont text straight into the frame buffer bytes, pixel for pixel
// what gfx().setFont(font); setCursor(x, y); print(s) draws with text size 1
// and wrap off (the same glyphs, offsets, cursor advance and clipping,
// without one virtual drawPixel call per set pixel). endX / endY get the
// cursor print() would leave. False, with nothing drawn, unless the buffer is
// directly addressable (panel::Display::frameView) at rotation 0 or 2; the
// caller then draws through gfx() as before. The caller owns the text size
// and wrap preconditions.
bool blitText(const GFXfont *font, int x, int y, const char *s, uint16_t color,
              int &endX, int &endY);
// True when blitText would draw right now.
bool blitAvailable();
// Canvas rows [y, y + h), full width, to white straight in the buffer bytes:
// what gfx().fillRect(0, y, width, h, INK_WHITE) draws, without one virtual
// drawPixel per pixel. False, with nothing drawn, where blitText is not
// available or the window is not full width; the caller then uses fillRect.
bool fillRowsWhite(int y, int h);
// The frame buffer bytes as the current window addresses them (stride times
// window rows), for speed read's identity check (bench `blitcheck`). False
// where blitText is not available.
bool frameBytes(uint8_t *&buf, size_t &len);
// True when frameBytes is laid out at rotation 2 (canvas row y is buffer row
// H - 1 - y, bytes and bits reversed along the row). Only meaningful where
// frameBytes succeeds.
bool frameFlipped();

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

// Speed read's incremental frames. The page height is the whole panel, so
// GxEPD2_BW keeps the buffer across nextPage(); only firstPage() clears it.
//
// frameBeginRawKeep: a whole screen partial like frameBeginRaw(false), but
// the buffer is NOT cleared: it still holds the last frame composed in it, to
// be drawn over. Only valid while frameEpoch() is what it was right after
// that frame, and that frame was not a window frame.
void frameBeginRawKeep();
// frameBeginRawWindow: a partial on the full width rows y .. y + h - 1 only
// (canvas coordinates; rows need no alignment on a landscape canvas, the full
// width is a byte multiple). GxEPD2_BW then addresses the buffer from the
// window's top left, so it is cleared here and the caller draws in canvas
// coordinates as usual (pixels outside the window are dropped). Only those
// rows are sent to the panel; the rest of the glass keeps what is on it. The
// buffer is then window shaped, so the next whole screen frame has to be
// composed from scratch (frameEpoch moves on). Only where windowFrames().
void frameBeginRawWindow(int y, int h);
// False on a driver that ignores the window and always ships a whole frame
// from the buffer (panel 0's T81 driver): no window frames there.
bool windowFrames();
// Moves on at every buffer clear (any frame's firstPage), rotation change and
// driver change.
uint32_t frameEpoch();

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

// Menu view: a grid of tiles under a header band carrying the sensor line,
// two columns on the portrait panels and as many as the width holds on a
// wider canvas (menuColumns). `cursor` indexes `tiles` row major (0 and 1 are
// the first row on two columns). The tile at the cursor is drawn inverted.
// Rows scroll to keep the cursor's row on screen when there are more tiles
// than fit.
void renderMenu(const String &header, const std::vector<MenuTile> &tiles,
                int cursor, bool forceFull = false);

// Grid shape, so the navigation code in main.cpp agrees with the renderer.
int menuColumns();

// Jump to page view: the target number is shown large.
void renderJump(const String &title, uint32_t target, uint32_t pageCount,
                bool forceFull = false);

// Full screen message (empty library, missing book, parse failure). Always a
// full refresh: these screens stay up for a long time.
//
// Lines are written for the narrowest text grid in the panel table (37
// columns), one per body text row. When there are more lines than the canvas
// has body rows (a short canvas: 10 on the 5.79 inch bar), they go on in the
// 7x14 status font instead, in as many side by side columns as it takes.
void renderMessage(const String &title, const std::vector<String> &lines);

// Same layout as renderMessage, but on the ordinary partial refresh policy.
// This is what the WiFi setup and news sync progress screens use: they redraw
// every few seconds and must not spend a full flash each time. The usual
// ration still applies, so a full refresh lands every PARTIALS_BEFORE_FULL
// updates and clears the ghosting.
void renderStatusScreen(const String &title, const std::vector<String> &lines,
                        bool forceFull = false);

}  // namespace ui
