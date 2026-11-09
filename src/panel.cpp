#include "panel.h"

#include <GxEPD2_BW.h>
#include <Preferences.h>
#include <SPI.h>
#include <esp_heap_caps.h>

#include <new>

#include "config.h"
#include "drivers/GxEPD2_576_T81_Fast.h"
#include "drivers/GxEPD2_579_Fast.h"
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
bool SHORT_CANVAS = false;
}  // namespace ui

namespace panel {
namespace {

// --------------------------------------------- GxEPD2_BW private state
//
// GxEPD2_BW keeps its frame buffer, partial window and flags private and has
// no accessor, and the library is not ours to edit. Display::frameView needs
// them read only, so they are reached through the one door C++ leaves open:
// access checking does not apply to the template arguments of an explicit
// instantiation ([temp.spec]). Each `template struct Steal<Tag, &B::member>`
// below stores the member pointer in Stolen<Tag>::ptr during static
// initialisation, before setup(). A pointer that is somehow still null makes
// frameView answer false, which is the GFX path, never a wrong write.
template <class B, class T, int K>
struct Field {
  typedef T B::*type;
};
template <class Tag>
struct Stolen {
  static typename Tag::type ptr;
};
template <class Tag>
typename Tag::type Stolen<Tag>::ptr = nullptr;
template <class Tag, typename Tag::type M>
struct Steal {
  struct Grab {
    Grab() { Stolen<Tag>::ptr = M; }
  };
  static Grab grab;
};
template <class Tag, typename Tag::type M>
typename Steal<Tag, M>::Grab Steal<Tag, M>::grab;

// The GxEPD2_BW of driver D with a whole frame per page, as below, and the
// tags of the members frameView reads.
template <class D>
using Bw = GxEPD2_BW<D, D::HEIGHT>;
template <class D>
using BufField = Field<Bw<D>, uint8_t[(D::WIDTH / 8) * D::HEIGHT], 0>;
template <class D, int K>
using U16Field = Field<Bw<D>, uint16_t, K>;  // 0..3: _pw_x, _pw_y, _pw_w, _pw_h
template <class D, int K>
using BoolField = Field<Bw<D>, bool, K>;     // 0: _mirror, 1: _reverse
template <class D>
using PageField = Field<Bw<D>, int16_t, 0>;  // _current_page

#define BW_STEAL(D)                                                   \
  template struct Steal<BufField<D>, &Bw<D>::_buffer>;                \
  template struct Steal<U16Field<D, 0>, &Bw<D>::_pw_x>;               \
  template struct Steal<U16Field<D, 1>, &Bw<D>::_pw_y>;               \
  template struct Steal<U16Field<D, 2>, &Bw<D>::_pw_w>;               \
  template struct Steal<U16Field<D, 3>, &Bw<D>::_pw_h>;               \
  template struct Steal<BoolField<D, 0>, &Bw<D>::_mirror>;            \
  template struct Steal<BoolField<D, 1>, &Bw<D>::_reverse>;           \
  template struct Steal<PageField<D>, &Bw<D>::_current_page>;

BW_STEAL(GxEPD2_576_T81_Fast)
BW_STEAL(GxEPD2_750_T7)
BW_STEAL(GxEPD2_579_Fast)
#undef BW_STEAL

// ------------------------------------------------------ driver wrappers
//
// One Display implementation per GxEPD2 driver class. The GxEPD2_BW page
// height is the full native panel height, so a whole frame is composed in one
// firstPage/nextPage pass (paging would re-run the text draw loop once per
// stripe). That makes the object as large as the frame: 920*680/8 = 78200
// bytes for panel 0, 800*480/8 = 48000 for panel 1, 792*272/8 = 26928 for
// panel 2.
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
  bool frameView(FrameView &v) override {
    const auto buf = Stolen<BufField<D>>::ptr;
    const auto pwX = Stolen<U16Field<D, 0>>::ptr;
    const auto pwY = Stolen<U16Field<D, 1>>::ptr;
    const auto pwW = Stolen<U16Field<D, 2>>::ptr;
    const auto pwH = Stolen<U16Field<D, 3>>::ptr;
    const auto mir = Stolen<BoolField<D, 0>>::ptr;
    const auto rev = Stolen<BoolField<D, 1>>::ptr;
    const auto page = Stolen<PageField<D>>::ptr;
    if (!buf || !pwX || !pwY || !pwW || !pwH || !mir || !rev || !page) return false;
    // drawPixel's other transforms (mirror, the GDE0213B1 y reverse, paging)
    // are not modelled; nothing here uses them, so they only fall back.
    if (bw.*mir || bw.*rev || bw.pages() != 1 || bw.*page != 0) return false;
    v.buf = bw.*buf;
    v.winX = bw.*pwX;
    v.winY = bw.*pwY;
    v.winW = bw.*pwW;
    v.winH = bw.*pwH;
    if (v.winW % 8) return false;
    v.stride = v.winW / 8;
    v.nativeW = D::WIDTH_VISIBLE;
    v.nativeH = D::HEIGHT;
    return true;
  }

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
  void setDriveAll(bool on) override { bw.epd2.setDriveAll(on); }
  bool hasSpeedHooks() const override { return true; }
  bool honoursWindow() const override { return false; }
};

// The custom GDEY0579T93 driver, a copy of GxEPD2's stock class with speed
// read hooks, the only driver panel 2 runs on. Every hook but bulk SPI
// defaults to stock behaviour, so pages and menus refresh exactly as on the
// stock driver; ui::setSpeedDrive turns the speed profile's hooks on only
// while speed read flashes. It keeps no buffer of its own, so nothing to
// release here.
class Fast579Display : public BwDisplay<GxEPD2_579_Fast> {
 public:
  explicit Fast579Display(const GxEPD2_579_Fast &drv) : BwDisplay(drv) {}
  void setFastRefresh(bool fast) override { bw.epd2.setFastRefresh(fast); }
  void setForcedTemp(int t) override { bw.epd2.setForcedTemp(t); }
  void setCutMs(int ms) override { bw.epd2.setCutMs(ms); }
  void setDriveAll(bool on) override { bw.epd2.setDriveAll(on); }
  void setKeepPower(bool on) override { bw.epd2.setKeepPower(on); }
  void setSingleWrite(bool on) override { bw.epd2.setSingleWrite(on); }
  void setCustomLut(const uint8_t *lut, size_t len) override {
    bw.epd2.setCustomLut(lut, len);
  }
  void setSkipPrevious(bool on) override { bw.epd2.setSkipPrevious(on); }
  void setSourceLevels(uint8_t vsh1, uint8_t vsh2, uint8_t vsl) override {
    bw.epd2.setSourceLevels(vsh1, vsh2, vsl);
  }
  void setBulkSpi(bool on) override { bw.epd2.setBulkSpi(on); }
  bool bulkSpi() const override { return bw.epd2.bulkSpi(); }
  void setLeanTiming(bool on) override { bw.epd2.setLeanTiming(on); }
  bool leanTiming() const override { return bw.epd2.leanTiming(); }
  void setProfileLog(bool on) override { bw.epd2.setProfileLog(on); }
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

Display *createGdey0579() {
  return make<Fast579Display>(
      GxEPD2_579_Fast(PIN_EINK_CS, PIN_EINK_DC, PIN_EINK_RST, PIN_EINK_BUSY));
}

// ------------------------------------------------------------ the table
//
// Panel 0 keeps exactly the settings the single panel build had: 50 ms reset
// pulse (the SSD2677 does not come up reliably on the 2 ms pulse smaller
// Waveshare panels are happy with), 10 MHz SPI, speed read at a 60 ms cut.
// Its speed read clock, top speeds and full refresh cadence are the RSVP_*
// constants in config.h, so they stay where they were tuned.
//
// Panel 1 is the Waveshare 7.5 inch V2 (UC8179, GxEPD2_750_T7) on the
// Waveshare driver HAT, same SPI/CS/DC/RST/BUSY pins. The 2 ms reset pulse is
// what GxEPD2's examples use for Waveshare boards with the "clever" reset
// circuit, and 4 MHz is GxEPD2's own default clock: both conservative until
// tested on glass. Speed read is off until it has been tuned on this panel;
// its speed fields are placeholders (4 MHz, 120/240/360 wpm, 30/40 cadence).
//
// Panel 2 is the Good Display GDEY0579T93, a 5.79 inch 792x272 bar, driven
// through a DESPI-C02, same pins, by src/drivers/GxEPD2_579_Fast (a copy of
// GxEPD2's stock GxEPD2_579_GDEY0579T93 with speed read hooks; every build
// runs it since 2026-11-08, the stock class is no longer used). It is the one
// panel whose UI runs LANDSCAPE: an even rotation keeps the native frame as
// the canvas (2 since 2026-10-31: the v4 case holds the bar that way up, pad
// as wired), 792 wide by 272 tall, because a 272 px wide portrait page would
// hold 20 columns of text. The glass is two SSD1683 controllers in cascade,
// each driving one half of the width; the driver splits every write at native
// x = 396 (the slave takes the half nearest the FPC, the master the far half,
// mirrored) and the update commands run both at once, so the canvas is one
// 792 px frame here and the seam is the driver's business. Outside speed read
// it refreshes as the stock driver does: a real differential partial (~450 ms)
// and a fast full refresh (~2.2 s). The 10 ms reset pulse is GxEPD2's default
// for bare panels on a DESPI-C02, 10 MHz the clock the stock class is
// constructed with, and speed read runs a 20 MHz clock with no cut (the fast
// driver's cut is a stub). rsvp.cpp picks its short bar layout from the
// 272 px canvas height.
//
// Speed read on panel 2 runs one of two speed profiles (src/speed_profile.h),
// picked by the speed mode setting; ui::setSpeedDrive applies the profile and
// then any bench console override (src/bench.cpp) on top:
//   - Normal (default): the OTP partial waveform with keep power, single
//     write and lean timing, which removes the power ramps and the second
//     frame write from each flash. Caps 120/240/360 wpm by words per flash
//     (on glass 2026-11-09: it keeps up with about 120 wpm at 1 word).
//   - Turbo: a custom 8 frame 50 Hz NOHOLD LUT (~190 ms a flash with keep
//     power, single write and lean timing), DC balanced. On glass on the
//     founder's unit 2026-11-09: 8 frames gives near full blacks and almost
//     no ghosting (6 frames was greyer); HOLD dulled the blacks and ghosted
//     more, BALANCED flashed every pixel each flash. Real throughput is
//     ~315 wpm a word, so the caps are 300/600/900. Ghost clearing every
//     30/45 flashes is a soft clear (src/rsvp.cpp: black, then blank, both
//     drive all partials), since a real full left negatives of the next few
//     words under NOHOLD; a real full still runs about every 10 minutes and
//     on entry, each followed by a soft clear.
// The legacy speedWpmMax / speedFullAfter / speedFullMax fields carry
// Normal's values as the fallback for code that does not read a profile. The
// ghost clearing full costs ~2.2 s here, so it runs less often than on panel
// 0 (Normal 60/90, pending an on-glass ghosting check).
const speed::Profile GDEY0579_NORMAL = {
    "Normal", speed::LutKind::OTP, 0, 0,
    true, true, true, false,  // keep power, single write, lean, skip previous
    -1,                       // panel sensor picks the OTP waveform
    {0, 120, 240, 360},       // on glass 2026-11-09: ~120 wpm real at 1 word
    60, 90};
// skipPrevious is only honoured while the LUT in force is HOLD (ui.cpp): the
// NOHOLD and BALANCED tables read the previous frame. Kept true so a soak
// that settles on HOLD gets the tuned drive back unchanged.
const speed::Profile GDEY0579_TURBO = {
    "Turbo", speed::LutKind::NOHOLD, 8, 2,  // 8 frames: on glass 2026-11-09
    true, true, true, true,
    -1,
    {0, 300, 600, 900},
    30, 45};
#define PANEL2_WPM_MAX {0, 120, 240, 360}
const Spec SPECS[] = {
    {"Good Display 5.76\" GDEH0576T81", "5.76\" Good Display",
     GxEPD2_576_T81_Fast::WIDTH, GxEPD2_576_T81_Fast::HEIGHT, 1, 50,
     10000000, true, RSVP_CUT_MS, RSVP_SPI_HZ,
     {(uint16_t)RSVP_WPM_MAX_BY_WORDS[0], (uint16_t)RSVP_WPM_MAX_BY_WORDS[1],
      (uint16_t)RSVP_WPM_MAX_BY_WORDS[2], (uint16_t)RSVP_WPM_MAX_BY_WORDS[3]},
     RSVP_FULL_AFTER, RSVP_FULL_MAX, BOOK_FONT_PROFONT22, BOOK_FONT_PROFONT29,
     createT81, nullptr, nullptr},
    {"Waveshare 7.5\" V2 800x480", "7.5\" Waveshare V2",
     GxEPD2_750_T7::WIDTH, GxEPD2_750_T7::HEIGHT, 1, 2, 4000000, false, 0,
     4000000, {0, 120, 240, 360}, 30, 40, BOOK_FONT_PROFONT22,
     BOOK_FONT_PROFONT29, createWs750V2, nullptr, nullptr},
    {"Good Display 5.79\" GDEY0579T93", "5.79\" Good Display",
     GxEPD2_579_Fast::WIDTH, GxEPD2_579_Fast::HEIGHT, 2, 10,
     10000000, true, 0, 20000000, PANEL2_WPM_MAX, 60, 90,
     BOOK_FONT_PROFONT22, BOOK_FONT_PROFONT29, createGdey0579,
     &GDEY0579_NORMAL, &GDEY0579_TURBO},
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
  uint8_t id = DEFAULT_PANEL_ID;
  Preferences p;
  // Read only: a namespace that was never written fails to open, which is
  // the "never chosen" case and means the build's DEFAULT_PANEL_ID.
  if (p.begin(NVS_NS_UI, true)) {
    id = p.getUChar(NVS_KEY_UI_PANEL, DEFAULT_PANEL_ID);
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

// The native frame is landscape on every panel in the table. An odd UI
// rotation turns it on its side (portrait), an even one keeps it.
int canvasW(const Spec &s) { return (s.uiRotation & 1) ? s.nativeH : s.nativeW; }
int canvasH(const Spec &s) { return (s.uiRotation & 1) ? s.nativeW : s.nativeH; }

void applyGeometry() {
  const Spec &s = activeSpec();
  SCREEN_W = canvasW(s);
  SCREEN_H = canvasH(s);
  DISPLAY_ROTATION = s.uiRotation;
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
    const uint32_t w = canvasW(SPECS[i]);  // content width
    const uint32_t h = canvasH(SPECS[i]) - STATUS_STRIP_H;
    const uint32_t bytes = ((w + 7) / 8) * h;
    if (bytes > most) most = bytes;
  }
  MPG1_MAX_IMAGE_BYTES = (most / 1000 + 1) * 1000;

  ui::GRID_BOTTOM = STATUS_RULE_Y - ui::GRID_BOTTOM_INSET;
  ui::TILE_W = (SCREEN_W - 2 * TEXT_MARGIN_X -
                ui::GRID_GUTTER * (ui::GRID_COLS - 1)) /
               ui::GRID_COLS;
  // Two rows of full size cards under the full header band, or the compact
  // forms of the menu and numbers views. See SHORT_CANVAS in ui.h.
  ui::SHORT_CANVAS = ui::GRID_BOTTOM - (ui::HEADER_H + ui::GRID_TOP_GAP) <
                     2 * ui::CARD_MIN_H + ui::GRID_GUTTER;
}

void destroy(Display *d) {
  if (!d) return;
  d->~Display();
  heap_caps_free(d);
}

}  // namespace panel
