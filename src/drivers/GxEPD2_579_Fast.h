// Display Library for SPI e-paper panels from Dalian Good Display and boards from Waveshare.
// Requires HW SPI and Adafruit_GFX. Caution: the e-paper panels require 3.3V supply AND data lines!
//
// based on Demo Example from Good Display, available here: https://www.good-display.com/comp/xcompanyFile/downloadNew.do?appId=24&fid=1781&id=1133
// Panel: GDEY0579T93 : https://www.good-display.com/product/439.html
// Controller : SSD1683 : https://v4.cecdn.yun300.cn/100001_1909185148/SSD1683.PDF
//
// Author: Jean-Marc Zingg
//
// Version: see library.properties
//
// Library: https://github.com/ZinggJM/GxEPD2
//
// ---------------------------------------------------------------------------
// GxEPD2_579_Fast: the wide reader's panel (panel 2) with speed read hooks.
//
// A copy of GxEPD2's stock gdey/GxEPD2_579_GDEY0579T93 (library 1.6.9), not a
// subclass: the stock helpers (_Update_Part, _setPartialRamArea*, ...) are
// private, so there is nothing to override. Renamed for the same reason as
// GxEPD2_576_T81_Fast (see its header): GxEPD2_BW.h compiles the stock class
// anyway, and a distinct name keeps the linker from having to choose. The
// statics and constructor match the stock class, so this drops into
// GxEPD2_BW<GxEPD2_579_Fast, GxEPD2_579_Fast::HEIGHT> unchanged. Panel 2's
// only driver since 2026-11-08 (every wide reader env, src/panel.cpp).
//
// Panel layout: two SSD1683s in cascade, each owning a 396 px wide half. The
// slave (left half, near the FPC) is addressed by OR-ing 0x80 into the
// command byte; the master (right half) is addressed with the plain command
// and drives the update timing for both (0x21 second byte 0x10 = cascade).
//
// Every hook below defaults OFF except setBulkSpi (on), and with all of them
// off the command stream is byte for byte the stock driver's. Bulk SPI keeps
// the bytes identical too; it only changes how they are handed to SPIClass. Speed read turns
// the hooks on per the panel's speed profile (src/panel.cpp, Normal/Turbo;
// src/ui.cpp setSpeedDrive). Keep power, single write, lean timing, the
// custom LUT and skip previous were tuned on the founder's unit 2026-10-27;
// the remaining "VERIFY on bench" notes in the .cpp are still open for a
// second panel.

#ifndef _GXEPD2_579_FAST_H_
#define _GXEPD2_579_FAST_H_

#include <GxEPD2_EPD.h>

class GxEPD2_579_Fast : public GxEPD2_EPD
{
  public:
    // attributes
    static const uint16_t WIDTH = 792;
    static const uint16_t WIDTH_VISIBLE = WIDTH;
    static const uint16_t HEIGHT = 272;
    static const GxEPD2::Panel panel = GxEPD2::GDEY0579T93;
    static const bool hasColor = false;
    static const bool hasPartialUpdate = true;
    static const bool hasFastPartialUpdate = true;
    static const bool useFastFullUpdate = true; // set false for extended (low) temperature range
    static const uint16_t power_on_time = 100; // ms, e.g. 96000us
    static const uint16_t power_off_time = 300; // ms, e.g. 268000us
    static const uint16_t full_refresh_time = 2200; // ms, e.g. 2183998us
    static const uint16_t partial_refresh_time = 450; // ms, e.g. 421000us
    // constructor
    GxEPD2_579_Fast(int16_t cs, int16_t dc, int16_t rst, int16_t busy);
    // methods (virtual)
    //  Support for Bitmaps (Sprites) to Controller Buffer and to Screen
    void clearScreen(uint8_t value = 0xFF); // init controller memory and screen (default white)
    void writeScreenBuffer(uint8_t value = 0xFF); // init controller memory (default white)
    void writeScreenBufferAgain(uint8_t value = 0xFF); // init previous buffer controller memory (default white)
    // write to controller memory, without screen refresh; x and w should be multiple of 8
    void writeImage(const uint8_t bitmap[], int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false);
    void writeImageForFullRefresh(const uint8_t bitmap[], int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false);
    void writeImageToPrevious(const uint8_t bitmap[], int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false);
    void writeImagePart(const uint8_t bitmap[], int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
                        int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false);
    void writeImagePartToPrevious(const uint8_t bitmap[], int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
                                int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false);
    void writeImage(const uint8_t* black, const uint8_t* color, int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false);
    void writeImagePart(const uint8_t* black, const uint8_t* color, int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
                        int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false);
    // for differential update: set current and previous buffers equal (for fast partial update to work correctly)
    void writeImageAgain(const uint8_t bitmap[], int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false);
    void writeImagePartAgain(const uint8_t bitmap[], int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
                             int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false);
    // write sprite of native data to controller memory, without screen refresh; x and w should be multiple of 8
    void writeNative(const uint8_t* data1, const uint8_t* data2, int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false);
    // write to controller memory, with screen refresh; x and w should be multiple of 8
    void drawImage(const uint8_t bitmap[], int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false);
    void drawImagePart(const uint8_t bitmap[], int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
                       int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false);
    void drawImage(const uint8_t* black, const uint8_t* color, int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false);
    void drawImagePart(const uint8_t* black, const uint8_t* color, int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
                       int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false);
    // write sprite of native data to controller memory, with screen refresh; x and w should be multiple of 8
    void drawNative(const uint8_t* data1, const uint8_t* data2, int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false);
    void refresh(bool partial_update_mode = false); // screen refresh from controller memory to full screen
    void refresh(int16_t x, int16_t y, int16_t w, int16_t h); // screen refresh from controller memory, partial screen
    void powerOff(); // turns off generation of panel driving voltages, avoids screen fading over time
    void hibernate(); // turns powerOff() and sets controller to deep sleep for minimum power use, ONLY if wakeable by RST (rst >= 0)

    // ---- Speed read hooks (all OFF by default = stock behaviour) ----------

    // Stored only. Unlike the T81 driver there is no separate fast LUT to
    // switch to: this panel's partial waveform is the OTP mode 2 LUT that
    // _Update_Part already uses, and GxEPD2_BW picks partial vs full itself
    // from setPartialWindow/setFullWindow. Kept so src/panel.cpp can drive
    // both fast drivers through the same calls.
    void setFastRefresh(bool fast) { _want_fast = fast; }
    bool isFastRefresh() const { return _want_fast; }
    // Forced LUT temperature for partial refreshes, in degrees C, or -1 for
    // the internal sensor (the default). When set, each partial writes the
    // temperature register (0x1A) and runs a 0x22 sequence without the "load
    // temperature" bit, so the OTP LUT is picked for the forced value, the
    // same trick the stock driver uses at 100 C for its fast full refresh.
    // Full refreshes are not affected. Clamped to 0..100.
    void setForcedTemp(int t);
    int forcedTemp() const { return _forced_temp; }
    // Waveform cut, in ms (0 = off). STUB: stored and logged once, nothing is
    // cut. See the .cpp (_Update_Part) for why a reset cut is risky with two
    // cascaded SSD1683s.
    void setCutMs(int ms);
    int cutMs() const { return _cut_ms; }
    // White push: while on, every partial write also loads the "previous"
    // RAM (0x26) with the inverse of the new frame, so every pixel reads as a
    // transition and the mode 2 LUT drives all of them (white pixels pushed
    // white, black pushed black), not just the changed ones. Costs one extra
    // frame write per flash. Stays on until cleared, like the T81 hook; the
    // caller turns it on for one flash and back off.
    void setDriveAll(bool on) { _drive_all = on; }
    bool driveAll() const { return _drive_all; }
    // Keep the analog supplies up between partials: power on once (0x22
    // 0xC0 on top of the stock _PowerOn), then run each partial with a 0x22
    // value that has neither the enable nor the disable bits, saving the
    // ~100 ms on / ~300 ms off ramp per flash. The 0x3C/0x21 cascade setup
    // still runs on every partial. Full refreshes, powerOff() and hibernate()
    // still power off. Do not leave this on while the reader idles: the panel
    // sits with its drive rails up.
    void setKeepPower(bool on);
    bool keepPower() const { return _keep_power; }
    // Skip the redundant half of writeImageAgain / writeImagePartAgain: the
    // 0x24 (current) write. See the .cpp for the condition that makes it
    // safe; the 0x26 (previous) write always runs.
    void setSingleWrite(bool on) { _single_write = on; }
    bool singleWrite() const { return _single_write; }
    // Custom waveform for partial refreshes via 0x32 (Write LUT register),
    // written to both controllers. nullptr (the default) = the OTP LUT.
    // Expected length: 227 bytes of 0x32 data, or 233 bytes where the last 6
    // are the vendor-sample tail (0x3F EOPT, 0x03 VGH, 0x04 VSH1/VSH2/VSL,
    // 0x2C VCOM), the same layout as SSD1683 4-gray samples. No LUT data
    // ships with this driver; the pointer must stay valid while set (it is
    // not copied). Lengths other than 227 or 233 are rejected (logged).
    void setCustomLut(const uint8_t *lut, size_t len);
    bool hasCustomLut() const { return _lut != nullptr; }
    // Source levels (0x04: VSH1, VSH2, VSL register codes) written to both
    // controllers right after a 227 byte custom LUT, so the bench can trade
    // voltage for frames. vsh1 0 = keep the OTP partial's levels. Only used
    // with the custom LUT; OTP partials and full refreshes are untouched.
    void setSourceLevels(uint8_t vsh1, uint8_t vsh2, uint8_t vsl);
    // Skip the previous (0x26) frame write while a custom LUT is in. Only
    // correct for a table that drives all four transitions independent of
    // the previous frame. The next write under the OTP LUT drives every
    // pixel once to resync.
    void setSkipPrevious(bool on);
    // Bulk SPI for the RAM writes (0x24 / 0x26 image and screen buffer
    // data): bytes are gathered into a small static buffer and sent with
    // SPIClass::writeBytes under the same transaction and CS instead of one
    // transfer() call per byte. Same bytes on the wire, ~65 ms -> ~11 ms per
    // full frame at 20 MHz. The one hook that defaults ON in this driver;
    // off = the stock per-byte loops. Commands and short register writes
    // are unchanged either way.
    void setBulkSpi(bool on) { _bulk_spi = on; }
    bool bulkSpi() const { return _bulk_spi; }
    // Lean timing (default OFF = stock waits). Removes host side waits that
    // the SSD1683 does not need; the command and data bytes are unchanged:
    //  - the delay(2) after every RAM window setup (0x11/0x44/0x45/0x4E/0x4F),
    //    10 per full screen partial (4 per RAM plane write, 2 in refresh)
    //  - the delay(1) "WDT yields" around each image write (6 per partial)
    //  - the busy wait's blind delay(1) before polling, replaced by a short
    //    spin until BUSY rises (max 2 ms), then the same poll as stock
    //  - with bulk SPI on, image rows are memcpy'd into the SPI buffer
    //    instead of computing every byte's index (same bytes)
    // FreeRTOS ticks at 1 kHz, so each delay(n) costs n-1..n ms plus any
    // task that runs meanwhile: ~15-25 ms per partial in total. See the .cpp
    // (_setPartialRamArea, _leanWaitWhileBusy) for the reasoning and what to
    // watch on glass.
    void setLeanTiming(bool on) { _lean = on; }
    bool leanTiming() const { return _lean; }

    // ---- Phase profiling ---------------------------------------------------
    // Always collected (a few micros() calls per partial). One cycle = what
    // GxEPD2_BW::nextPage does for one frame: writeImage (0x24, plus 0x26 if
    // drive all) -> refresh -> writeImageAgain (0x26, plus 0x24 unless single
    // write). The cycle opens at the first image write and closes at the end
    // of the *Again call that follows a refresh. All times in microseconds.
    struct Profile
    {
      uint32_t setupUs;   // RAM window setups (0x11/0x44/0x45/0x4E/0x4F), incl. their delay(2)
      uint32_t yieldUs;   // the delay(1) WDT yields in the image write paths
      uint32_t write24Us; // 0x24 (current) writes: command + data bytes
      uint32_t write26Us; // 0x26 (previous) writes: command + data bytes
      uint32_t cmdUs;     // update commands: custom LUT, 0x3C, 0x21, 0x1A, 0x22, 0x20
      uint32_t powerUs;   // _PowerOn wait inside the cycle (keep power: first partial only)
      uint32_t busyUs;    // update BUSY wait; stock mode also includes the 1 ms margin and the diag print
      uint32_t otherUs;   // totalUs minus everything above (caller time between calls, diag print in lean mode)
      uint32_t totalUs;   // first image write start .. end of the closing *Again call
      uint32_t bytes;     // RAM data bytes sent (both planes, both controllers)
      uint16_t setups;    // number of RAM window setups
      bool full;          // the cycle ran a full refresh (_Update_Full) instead of a partial
      bool lean;          // setLeanTiming was on
      uint32_t n;         // cycles completed since boot (0 = none yet)
    };
    // The last completed cycle (all zero before the first one).
    Profile lastProfile() const { return _prof_last; }
    // When on, Serial.printf one line per completed cycle, e.g.
    // "579prof n=12 P lean=1 tot=52.3 setup=0.4/10 yld=0.0 w24=12.1 w26=12.0 cmd=0.1 pwr=0.0 busy=281.2 oth=0.3 ms bytes=53856"
    // Default off.
    void setProfileLog(bool on) { _prof_log = on; }
    bool profileLog() const { return _prof_log; }
  private:
    void _yield1();
    uint32_t _leanWaitWhileBusy(const char *comment, uint16_t busy_time);
    void _profBegin();
    void _profEnd();
    void _profWrite(uint8_t command, uint32_t t0, uint32_t setup0, uint32_t yield0);
    bool _lean = false;
    bool _prof_log = false;
    bool _prof_open = false;
    bool _prof_updated = false;
    uint32_t _prof_t0 = 0;
    Profile _prof_cur = {};
    Profile _prof_last = {};
    void _transferFill(uint8_t value, uint32_t n);
    void _writeScreenBuffer(uint8_t command, uint8_t value);
    void _writeImage(uint8_t command, const uint8_t bitmap[], int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false);
    void _writeFromImage(uint8_t command, const uint8_t bitmap[], int16_t x1, int16_t y1, int16_t w1, int16_t h1, int16_t wbb, int16_t hb, int16_t dx, int16_t dy, bool invert, bool mirror_y, bool pgm);
    void _writeDataFromImage(const uint8_t bitmap[], int16_t w, int16_t h, int16_t wbb, int16_t hb, int16_t dx, int16_t dy, bool invert, bool mirror_y, bool pgm);
    void _writeImagePart(uint8_t command, const uint8_t bitmap[], int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
                         int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false);
    void _setPartialRamAreaMaster(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t mode = 0x03);
    void _setPartialRamAreaSlave(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t mode = 0x03);
    void _setPartialRamArea(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t mode, uint8_t target);
    void _PowerOn();
    void _PowerOff();
    void _InitDisplay();
    void _Update_Full();
    void _Update_Part();
    void _writeCustomLut();
    bool _want_fast = false;
    int _forced_temp = -1;
    int _cut_ms = 0;
    bool _cut_logged = false;
    bool _drive_all = false;
    bool _keep_power = false;
    bool _single_write = false;
    bool _bulk_spi = true; // the one hook on by default, see setBulkSpi
    const uint8_t *_lut = nullptr;
    size_t _lut_len = 0;
    // True once _lut is in both controllers' LUT registers. Any OTP LUT load
    // (init, full refresh) overwrites the register, so those clear it.
    bool _lut_loaded = false;
    uint8_t _vsh1 = 0, _vsh2 = 0, _vsl = 0; // setSourceLevels, 0 = OTP
    bool _skip_prev = false;
    bool _prev_stale = false; // 0x26 not written since a skipped frame
};

#endif
