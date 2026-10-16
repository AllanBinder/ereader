#pragma once

// Supported e-paper panels and the runtime choice between them.
//
// One firmware image drives every panel in the table below. The choice lives
// in NVS (namespace NVS_NS_UI, key NVS_KEY_UI_PANEL, u8) so it survives a
// reflash and an uploadfs; a reader that has never chosen has no key and runs
// panel 0, the Good Display GDEH0576T81 every earlier build was written for.
//
// At boot ui::begin() reads the choice, fills the screen geometry globals in
// config.h and ui.h (panel::applyGeometry), and creates exactly one display
// driver on the heap through the factory in the table, so only the chosen
// panel's frame buffer is ever allocated. ui::beginPanel() repeats that for
// another id without a reboot, which is what the boot-time picker uses to
// cycle through the panels.
//
// Adding a panel: one row in the table in panel.cpp (a GxEPD2 driver class
// whose native frame is landscape, so an odd rotation makes it portrait), plus
// on-glass testing. Layout and text grids are derived from the canvas size, so
// nothing else in the firmware names a panel.

#include <Adafruit_GFX.h>
#include <Arduino.h>

namespace panel {

// Panel ids. Stored in NVS, so never renumber a shipped id.
static const uint8_t GDEH0576T81 = 0;  // Good Display 5.76", DESPI-C02
static const uint8_t WS_750_V2 = 1;    // Waveshare 7.5" V2, 800x480

// The thin display interface src/ui.cpp draws through, so it never names a
// GxEPD2 driver type. One implementation per driver class lives in panel.cpp,
// wrapping GxEPD2_BW<Driver, Driver::HEIGHT> (a whole frame per page pass).
// The last four hooks are the custom GDEH0576T81 driver's speed read and
// refresh controls; on a stock GxEPD2 driver they do nothing.
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
  // Custom driver only (no-ops elsewhere). setFastRefresh picks the partial
  // LUT for the next frame; the other three are the speed reader's drive.
  virtual void setFastRefresh(bool fast) { (void)fast; }
  virtual void setForcedTemp(int t) { (void)t; }
  virtual void setCutMs(int ms) { (void)ms; }
  // True when the hooks above really drive the panel.
  virtual bool hasSpeedHooks() const { return false; }
};

// One row of the panel table.
struct Spec {
  const char *name;       // full name, for the picker list
  const char *shortName;  // menu sub line, fits a tile
  uint16_t nativeW;       // driver frame, landscape
  uint16_t nativeH;
  uint8_t portraitRotation;  // 1 or 3; seeds DISPLAY_ROTATION
  uint16_t resetMs;          // reset pulse handed to init()
  uint32_t spiHz;            // SPI clock outside speed read
  bool speedReadSupported;   // speed read tested on this glass
  int speedCutMs;            // speed read waveform cut (0 = none)
  uint8_t normalFont;        // BOOK_FONT_* of the normal text grid
  uint8_t largeFont;         // BOOK_FONT_* of the large text grid
  // Builds the driver and its frame buffer on the heap; nullptr when out of
  // memory. Free with destroy().
  Display *(*create)();
};

uint8_t count();
const Spec &spec(uint8_t id);
const Spec &activeSpec();
const char *name(uint8_t id);
const char *shortName(uint8_t id);

// Reads the stored choice (missing or out of range = 0) and makes it the
// active panel. Does not touch the display; ui::begin() calls it.
uint8_t load();
// Persists `id` as the choice for the next boot. Does not switch the live
// panel (ui::beginPanel does that). False when `id` is unknown or NVS failed.
bool set(uint8_t id);
// Makes `id` the active panel for this boot without persisting it.
void use(uint8_t id);
uint8_t active();
bool speedReadSupported();

// Fills the size-dependent globals (SCREEN_W and friends in config.h,
// ui::GRID_BOTTOM and ui::TILE_W in ui.h) from the active panel. Must run
// before anything draws, and again after use() picks another panel.
void applyGeometry();

// Frees a driver made by Spec::create, frame buffer included.
void destroy(Display *d);

}  // namespace panel
