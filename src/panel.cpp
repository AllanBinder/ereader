#include "panel.h"

#include <GxEPD2_BW.h>
#include <Preferences.h>
#include <SPI.h>
#include <esp_heap_caps.h>

#include <new>

#include "config.h"
#include "drivers/GxEPD2_576_T81_Fast.h"
#include "pins.h"
#include "ui.h"

// ------------------------------------------------------- geometry globals
//
// Declared extern in config.h and ui.h. The initial values are panel 0's, the
// layout every earlier build was compiled with, so anything that reads one
// before setup() (a file scope initializer in another file) still gets a
// sensible 5.76 inch number. applyGeometry() overwrites them all.
int SCREEN_W = 680;
int SCREEN_H = 920;
uint8_t DISPLAY_ROTATION = 1;
int STATUS_RULE_Y = 896;
int STATUS_BASELINE_Y = 912;
int CONTENT_W = 680;
int CONTENT_H = 896;
int TEXT_COLS = 54;
int TEXT_ROWS = 37;
int TEXT_LARGE_COLS = 40;
int TEXT_LARGE_ROWS = 27;
uint32_t MPG1_MAX_IMAGE_BYTES = 77000;

namespace ui {
int GRID_BOTTOM = 880;
int TILE_W = 318;
}  // namespace ui

namespace panel {
namespace {

// ------------------------------------------------------ driver wrappers
//
// One Display implementation per GxEPD2 driver class. The GxEPD2_BW page
// height is the full native panel height, so a whole frame is composed in one
// firstPage/nextPage pass (paging would re-run the text draw loop once per
// stripe). That makes the object as large as the frame: 920*680/8 = 78200
// bytes for panel 0, 800*480/8 = 48000 for panel 1.
template <class D>
class BwDisplay : public Display {
 public:
  explicit BwDisplay(const D &drv) : bw(drv) {}
  Adafruit_GFX &gfx() override { return bw; }
  void selectSPI(uint32_t hz) override {
    bw.epd2.selectSPI(SPI, SPISettings(hz, MSBFIRST, SPI_MODE0));
  }
  void setBusyCallback(void (*cb)(const void *)) override {
    bw.epd2.setBusyCallback(cb);
  }
  void init(uint32_t serialDiag, bool initial, uint16_t resetMs,
            bool pulldownRst) override {
    bw.init(serialDiag, initial, resetMs, pulldownRst);
  }
  void setRotation(uint8_t r) override { bw.setRotation(r); }
  uint8_t getRotation() override { return bw.getRotation(); }
  int16_t width() override { return bw.width(); }
  int16_t height() override { return bw.height(); }
  void setFullWindow() override { bw.setFullWindow(); }
  void setPartialWindow(uint16_t x, uint16_t y, uint16_t w,
                        uint16_t h) override {
    bw.setPartialWindow(x, y, w, h);
  }
  void firstPage() override { bw.firstPage(); }
  bool nextPage() override { return bw.nextPage(); }
  void fillScreen(uint16_t color) override { bw.fillScreen(color); }
  void hibernate() override { bw.hibernate(); }

 protected:
  GxEPD2_BW<D, D::HEIGHT> bw;
};

// The custom GDEH0576T81 driver: real partial LUT, forced LUT temperature and
// waveform cut for speed read. Its previous-frame shadow is malloc'd by the
// driver on the first write and has no destructor of its own (the driver is
// copied by value into GxEPD2_BW), so it is released here.
class T81Display : public BwDisplay<GxEPD2_576_T81_Fast> {
 public:
  explicit T81Display(const GxEPD2_576_T81_Fast &drv) : BwDisplay(drv) {}
  ~T81Display() override { bw.epd2.releaseShadow(); }
  void setFastRefresh(bool fast) override { bw.epd2.setFastRefresh(fast); }
  void setForcedTemp(int t) override { bw.epd2.setForcedTemp(t); }
  void setCutMs(int ms) override { bw.epd2.setCutMs(ms); }
  bool hasSpeedHooks() const override { return true; }
};

// The frame buffer is placed in internal RAM on purpose. It was a static
// before runtime panel selection, so it always lived there, and with PSRAM
// enabled (lolin_s3_mini sets BOARD_HAS_PSRAM) a plain malloc of this size
// would prefer PSRAM and slow every pixel write. PSRAM is only the fallback
// for a heap too fragmented to hold it, which should not happen this early in
// boot (ui::begin runs before LittleFS, sensors and WiFi).
template <class T, class D>
Display *make(const D &drv) {
  void *mem = heap_caps_malloc(sizeof(T), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!mem) mem = heap_caps_malloc(sizeof(T), MALLOC_CAP_8BIT);
  if (!mem) return nullptr;
  return new (mem) T(drv);
}

Display *createT81() {
  return make<T81Display>(GxEPD2_576_T81_Fast(PIN_EINK_CS, PIN_EINK_DC,
                                              PIN_EINK_RST, PIN_EINK_BUSY));
}

Display *createWs750V2() {
  return make<BwDisplay<GxEPD2_750_T7>>(
      GxEPD2_750_T7(PIN_EINK_CS, PIN_EINK_DC, PIN_EINK_RST, PIN_EINK_BUSY));
}

// ------------------------------------------------------------ the table
//
// Panel 0 keeps exactly the settings the single panel build had: 50 ms reset
// pulse (the SSD2677 does not come up reliably on the 2 ms pulse smaller
// Waveshare panels are happy with), 10 MHz SPI, speed read at a 60 ms cut.
//
// Panel 1 is the Waveshare 7.5 inch V2 (UC8179, GxEPD2_750_T7) on the
// Waveshare driver HAT, same SPI/CS/DC/RST/BUSY pins. The 2 ms reset pulse is
// what GxEPD2's examples use for Waveshare boards with the "clever" reset
// circuit, and 4 MHz is GxEPD2's own default clock: both conservative until
// tested on glass. Speed read is off until it has been tuned on this panel.
const Spec SPECS[] = {
    {"Good Display 5.76\" GDEH0576T81", "5.76\" Good Display",
     GxEPD2_576_T81_Fast::WIDTH, GxEPD2_576_T81_Fast::HEIGHT, 1, 50,
     10000000, true, RSVP_CUT_MS, BOOK_FONT_PROFONT22, BOOK_FONT_PROFONT29,
     createT81},
    {"Waveshare 7.5\" V2 800x480", "7.5\" Waveshare V2",
     GxEPD2_750_T7::WIDTH, GxEPD2_750_T7::HEIGHT, 1, 2, 4000000, false, 0,
     BOOK_FONT_PROFONT22, BOOK_FONT_PROFONT29, createWs750V2},
};
const uint8_t SPEC_COUNT = sizeof(SPECS) / sizeof(SPECS[0]);

uint8_t activeId = 0;

// Font metrics behind the text grid, by book font id. See the TEXT_* block in
// config.h for the arithmetic and the values it yields on each panel.
struct FontMetrics {
  int advance;
  int firstBaseline;
  int pitch;
  int descent;
};

FontMetrics metricsFor(uint8_t fontId) {
  if (fontId == BOOK_FONT_PROFONT29) {
    return FontMetrics{TEXT_LARGE_ADVANCE, TEXT_LARGE_FIRST_BASELINE_Y,
                       TEXT_LARGE_LINE_PITCH, TEXT_LARGE_DESCENT};
  }
  return FontMetrics{TEXT_ADVANCE, TEXT_FIRST_BASELINE_Y, TEXT_LINE_PITCH,
                     TEXT_DESCENT};
}

int gridCols(uint8_t fontId, int canvasW) {
  return (canvasW - 2 * TEXT_MARGIN_X) / metricsFor(fontId).advance;
}

// The most baselines whose descender ends at least one pixel above the rule.
int gridRows(uint8_t fontId, int ruleY) {
  const FontMetrics m = metricsFor(fontId);
  return (ruleY - 1 - m.descent - m.firstBaseline) / m.pitch + 1;
}

}  // namespace

uint8_t count() { return SPEC_COUNT; }

const Spec &spec(uint8_t id) { return SPECS[id < SPEC_COUNT ? id : 0]; }

const Spec &activeSpec() { return SPECS[activeId]; }

const char *name(uint8_t id) { return spec(id).name; }

const char *shortName(uint8_t id) { return spec(id).shortName; }

uint8_t load() {
  uint8_t id = 0;
  Preferences p;
  // Read only: a namespace that was never written fails to open, which is
  // the "never chosen" case and means panel 0.
  if (p.begin(NVS_NS_UI, true)) {
    id = p.getUChar(NVS_KEY_UI_PANEL, 0);
    p.end();
  }
  if (id >= SPEC_COUNT) id = 0;
  activeId = id;
  return id;
}

bool set(uint8_t id) {
  if (id >= SPEC_COUNT) return false;
  Preferences p;
  if (!p.begin(NVS_NS_UI, false)) return false;
  bool ok = p.putUChar(NVS_KEY_UI_PANEL, id) == 1;
  p.end();
  return ok;
}

void use(uint8_t id) { activeId = id < SPEC_COUNT ? id : 0; }

uint8_t active() { return activeId; }

bool speedReadSupported() { return activeSpec().speedReadSupported; }

void applyGeometry() {
  const Spec &s = activeSpec();
  // Portrait: the native frame is landscape on every panel in the table, so
  // the canvas is the native frame turned on its side.
  SCREEN_W = s.nativeH;
  SCREEN_H = s.nativeW;
  DISPLAY_ROTATION = s.portraitRotation;
  STATUS_RULE_Y = SCREEN_H - STATUS_STRIP_H;
  STATUS_BASELINE_Y = SCREEN_H - STATUS_BASELINE_INSET;
  CONTENT_W = SCREEN_W;
  CONTENT_H = STATUS_RULE_Y;
  TEXT_COLS = gridCols(s.normalFont, SCREEN_W);
  TEXT_ROWS = gridRows(s.normalFont, STATUS_RULE_Y);
  TEXT_LARGE_COLS = gridCols(s.largeFont, SCREEN_W);
  TEXT_LARGE_ROWS = gridRows(s.largeFont, STATUS_RULE_Y);

  // Image ceiling over every panel, not just this one; see config.h.
  uint32_t most = 0;
  for (uint8_t i = 0; i < SPEC_COUNT; i++) {
    const uint32_t w = SPECS[i].nativeH;  // portrait content width
    const uint32_t h = SPECS[i].nativeW - STATUS_STRIP_H;
    const uint32_t bytes = ((w + 7) / 8) * h;
    if (bytes > most) most = bytes;
  }
  MPG1_MAX_IMAGE_BYTES = (most / 1000 + 1) * 1000;

  ui::GRID_BOTTOM = STATUS_RULE_Y - ui::GRID_BOTTOM_INSET;
  ui::TILE_W = (SCREEN_W - 2 * TEXT_MARGIN_X -
                ui::GRID_GUTTER * (ui::GRID_COLS - 1)) /
               ui::GRID_COLS;
}

void destroy(Display *d) {
  if (!d) return;
  d->~Display();
  heap_caps_free(d);
}

}  // namespace panel
