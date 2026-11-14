#pragma once

// Supported e-paper panels and the runtime choice between them.
//
// One firmware image drives every panel in the table below. The choice lives
// in NVS (namespace NVS_NS_UI, key NVS_KEY_UI_PANEL, u8) so it survives a
// reflash and an uploadfs; a reader that has never chosen has no key and runs
// DEFAULT_PANEL_ID, which is panel 0 (the Good Display GDEH0576T81 every
// earlier build was written for) unless the PlatformIO env says otherwise:
// [env:esp32s3_579] builds with -DDEFAULT_PANEL_ID=2 so a fresh board with
// empty NVS comes up on the 5.79 inch bar panel without the boot picker.
//
// At boot ui::begin() reads the choice, fills the screen geometry globals in
// config.h and ui.h (panel::applyGeometry), and creates exactly one display
// driver on the heap through the factory in the table, so only the chosen
// panel's frame buffer is ever allocated. ui::beginPanel() repeats that for
// another id without a reboot, which is what the boot-time picker uses to
// cycle through the panels.
//
// Adding a panel: one row in the table in panel.cpp (a GxEPD2 driver class
// and the rotation its UI runs at: odd turns a landscape native frame into a
// portrait canvas, even keeps the native frame as the canvas), plus on-glass
// testing. Layout and text grids are derived from the canvas size, so nothing
// else in the firmware names a panel.

#include <Adafruit_GFX.h>
#include <Arduino.h>

#include "speed_profile.h"

namespace panel {

// The GxEPD2_BW frame buffer as drawPixel addresses it, for fast blits that
// bypass the per pixel virtual call (ui::blitText). A canvas pixel is first
// turned to native coordinates by the rotation (0: as is; 2: nx = nativeW - 1
// - x, ny = nativeH - 1 - y), then taken relative to the window and dropped
// outside it; bit (7 - bx % 8) of buf[by * stride + bx / 8] is the pixel,
// 1 = white, 0 = black.
struct FrameView {
  uint8_t *buf;
  uint16_t winX, winY, winW, winH;  // the window, native coordinates
  uint16_t stride;                  // bytes per buffer row, winW / 8
  uint16_t nativeW, nativeH;        // the GFX raw size (rotation 0)
};

// Panel ids. Stored in NVS, so never renumber a shipped id.
static const uint8_t GDEH0576T81 = 0;  // Good Display 5.76", DESPI-C02
static const uint8_t WS_750_V2 = 1;    // Waveshare 7.5" V2, 800x480
static const uint8_t GDEY0579T93 = 2;  // Good Display 5.79", 792x272 bar

// The panel a board with no stored choice runs. Set per PlatformIO env with
// -DDEFAULT_PANEL_ID=<id>; an id the table does not have falls back to 0.
#ifndef DEFAULT_PANEL_ID
#define DEFAULT_PANEL_ID 0
#endif

// The thin display interface src/ui.cpp draws through, so it never names a
// GxEPD2 driver type. One implementation per driver class lives in panel.cpp,
// wrapping GxEPD2_BW<Driver, Driver::HEIGHT> (a whole frame per page pass).
// The hooks at the end are the speed read and refresh controls of the custom
// drivers (GDEH0576T81 and GDEY0579T93); on a stock GxEPD2 driver (panel 1)
// they do nothing.
class Display {
 public:
  virtual ~Display() {}
  // The panel as a GFX canvas: every draw call goes through this.
  virtual Adafruit_GFX &gfx() = 0;
  // Bus setup that has to precede init(): the SPI clock and the busy poll
  // callback (fed by ui.cpp to keep the task watchdog quiet).
  virtual void selectSPI(uint32_t hz) = 0;
  virtual void setBusyCallback(void (*cb)(const void *)) = 0;
  // GxEPD2_BW::init(diag bitrate, initial, reset pulse ms, pulldown rst).
  virtual void init(uint32_t serialDiag, bool initial, uint16_t resetMs,
                    bool pulldownRst) = 0;
  virtual void setRotation(uint8_t r) = 0;
  virtual uint8_t getRotation() = 0;
  virtual int16_t width() = 0;
  virtual int16_t height() = 0;
  virtual void setFullWindow() = 0;
  virtual void setPartialWindow(uint16_t x, uint16_t y, uint16_t w,
                                uint16_t h) = 0;
  virtual void firstPage() = 0;
  virtual bool nextPage() = 0;
  virtual void fillScreen(uint16_t color) = 0;
  virtual void hibernate() = 0;
  // Custom drivers only (no-ops elsewhere). setFastRefresh picks the partial
  // LUT for the next frame; the other three are the speed reader's drive.
  virtual void setFastRefresh(bool fast) { (void)fast; }
  virtual void setForcedTemp(int t) { (void)t; }
  virtual void setCutMs(int ms) { (void)ms; }
  virtual void setDriveAll(bool on) { (void)on; }
  // Speed profile hooks of the GDEY0579T93 fast driver only, set by
  // ui::setSpeedDrive from the speed profile and the bench console
  // (src/bench.cpp): analog rails kept up between partials, single frame
  // write per partial, and a custom partial LUT (nullptr = OTP). No-ops on
  // every other driver, the T81 one included.
  virtual void setKeepPower(bool on) { (void)on; }
  virtual void setSingleWrite(bool on) { (void)on; }
  virtual void setCustomLut(const uint8_t *lut, size_t len) {
    (void)lut;
    (void)len;
  }
  // Skip the previous frame write under a custom LUT (fast 579 only).
  virtual void setSkipPrevious(bool on) { (void)on; }
  // 0x04 source level codes sent with the custom LUT (0 = OTP levels).
  virtual void setSourceLevels(uint8_t vsh1, uint8_t vsh2, uint8_t vsl) {
    (void)vsh1;
    (void)vsh2;
    (void)vsl;
  }
  // Bulk SPI (fast GDEY0579T93 driver only): frame writes go out as buffered
  // rows instead of one SPI call per byte. That driver turns it on by
  // default; the bench console toggles it. No-op and false elsewhere.
  virtual void setBulkSpi(bool on) { (void)on; }
  virtual bool bulkSpi() const { return false; }
  // Lean timing and per-partial profile lines (fast GDEY0579T93 driver only,
  // both default off there): lean drops the stock settle delays around the
  // RAM window setup; the profile prints one phase breakdown per partial.
  virtual void setLeanTiming(bool on) { (void)on; }
  virtual bool leanTiming() const { return false; }
  virtual void setProfileLog(bool on) { (void)on; }
  // True when the hooks above really drive the panel.
  virtual bool hasSpeedHooks() const { return false; }
  // True when a partial window smaller than the screen sends and refreshes
  // only the window's rows from a buffer addressed in window coordinates (the
  // GxEPD2_BW contract). The T81 driver ignores the window rectangle and
  // always ships a whole native frame, so it says false.
  virtual bool honoursWindow() const { return true; }
  // The frame buffer's layout (see FrameView), or false when it cannot be
  // addressed directly: no access, a mirrored or reversed GxEPD2_BW, or a
  // paged buffer. Valid until the next window, rotation or driver change.
  virtual bool frameView(FrameView &v) {
    (void)v;
    return false;
  }
};

// One row of the panel table.
struct Spec {
  const char *name;       // full name, for the picker list
  const char *shortName;  // menu sub line, fits a tile
  uint16_t nativeW;       // driver frame, landscape
  uint16_t nativeH;
  // Rotation the UI runs at as built; seeds DISPLAY_ROTATION. Odd (1 or 3)
  // turns the landscape native frame into a portrait canvas, nativeH wide;
  // even (0 or 2) keeps the native frame as a landscape canvas, nativeW wide.
  // "Flip screen" toggles between this and the rotation 180 degrees from it.
  uint8_t uiRotation;
  uint16_t resetMs;          // reset pulse handed to init()
  uint32_t spiHz;            // SPI clock outside speed read
  bool speedReadSupported;   // speed read tested on this glass
  int speedCutMs;            // speed read waveform cut (0 = none)
  uint32_t speedSpiHz;       // SPI clock during speed read
  uint16_t speedWpmMax[4];   // speed read top wpm by words per flash (1-3)
  uint8_t speedFullAfter;    // ghost clearing full at a sentence end after this
  uint8_t speedFullMax;      // and unconditionally after this many partials
  uint8_t normalFont;        // BOOK_FONT_* of the normal text grid
  uint8_t largeFont;         // BOOK_FONT_* of the large text grid
  // Builds the driver and its frame buffer on the heap; nullptr when out of
  // memory. Free with destroy().
  Display *(*create)();
  // Speed read drive profiles by speed mode (src/speed_profile.h), or
  // nullptr on a panel without them (0 and 1), which keeps the speed fields
  // above and its fixed drive, and hides the mode setting. When set, the
  // speed fields above carry the Normal profile's values as a fallback.
  const speed::Profile *speedNormal;
  const speed::Profile *speedTurbo;
  const speed::Profile *speedSuper;  // nullptr = no Super Turbo on this panel
};

uint8_t count();
const Spec &spec(uint8_t id);
const Spec &activeSpec();
const char *name(uint8_t id);
const char *shortName(uint8_t id);

// Reads the stored choice (missing = DEFAULT_PANEL_ID, out of range = 0) and
// makes it the active panel. Does not touch the display; ui::begin() calls it.
uint8_t load();
// Persists `id` as the choice for the next boot. Does not switch the live
// panel (ui::beginPanel does that). False when `id` is unknown or NVS failed.
bool set(uint8_t id);
// Makes `id` the active panel for this boot without persisting it.
void use(uint8_t id);
uint8_t active();
bool speedReadSupported();

// The canvas a panel's UI runs on, from its native frame and uiRotation:
// 680x920 for panel 0, 480x800 for panel 1, 792x272 for panel 2.
int canvasW(const Spec &s);
int canvasH(const Spec &s);

// Fills the size-dependent globals (SCREEN_W and friends in config.h,
// ui::GRID_BOTTOM and ui::TILE_W in ui.h) from the active panel. Must run
// before anything draws, and again after use() picks another panel.
void applyGeometry();

// Frees a driver made by Spec::create, frame buffer included.
void destroy(Display *d);

}  // namespace panel
