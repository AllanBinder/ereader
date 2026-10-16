// GxEPD2_576_T81_Fast: Good Display GDEH0576T81 with flash-free partial refresh.
//
// Vendored from the Muon Sortes firmware
// (Muon_Decider/src/drivers/GxEPD2_576_GDEH0576T81.{h,cpp}), which is itself a
// rewrite of GxEPD2's stock gdeh/GxEPD2_576_GDEH0576T81 driver. The stock
// driver declares hasFastPartialUpdate = false and runs the slow full waveform
// on every setPartialWindow frame, so a page turn or a menu cursor move costs a
// full multi-second flash. This one carries its own partial LUT (_Init_Part)
// plus a shadow copy of the previous frame, and interleaves old and new pixels
// so the panel only drives the pixels that actually changed.
//
// Why a new class name rather than shadowing the stock header: GxEPD2_BW.h
// pulls in every panel header it can see, including the stock
// gdeh/GxEPD2_576_GDEH0576T81.h, and both that header and the library's
// matching .cpp are compiled into the build no matter what we do. Reusing the
// stock class name would mean relying on include order and on the linker
// preferring our object over the archive member. Renaming the class removes the
// ambiguity outright: the stock class is still compiled, but nothing
// instantiates it and the linker drops it. GxEPD2_BW is a template, so it takes
// this class exactly as it took the stock one.
//
// 5.76" E Ink Carta HD, 920x680, B/W, UC81xx family controller.
// 2bpp pixel format on the wire (each 1bpp byte becomes 2 output bytes).
// BUSY active LOW.
//
// Differential refresh via bit-interleaved old+new pixel data:
//   Each pixel is 2 bits on the wire: [old_value, new_value].
//   The partial LUT drives only transitions (01 = black to white,
//   10 = white to black). Unchanged pixels (00, 11) are not driven.
//   A shadow buffer on the ESP32 tracks the previous frame for interleaving.
//
// Note that writeImage() here always consumes a full 920x680 frame and ignores
// the window rectangle, so the GxEPD2_BW page height must be the full panel
// height and the partial window must be the whole screen. src/ui.cpp does both.

#ifndef _GXEPD2_576_T81_FAST_H_
#define _GXEPD2_576_T81_FAST_H_

#include <GxEPD2_EPD.h>

class GxEPD2_576_T81_Fast : public GxEPD2_EPD
{
  public:
    static const uint16_t WIDTH = 920;
    static const uint16_t WIDTH_VISIBLE = WIDTH;
    static const uint16_t HEIGHT = 680;
    static const GxEPD2::Panel panel = GxEPD2::GDEM102T91; // reuse enum slot
    static const bool hasColor = false;
    static const bool hasPartialUpdate = true;
    static const bool hasFastPartialUpdate = false;
    static const uint16_t power_on_time = 5000;
    static const uint16_t power_off_time = 1000;
    static const uint16_t full_refresh_time = 25000;
    static const uint16_t partial_refresh_time = 5000;
    // constructor
    GxEPD2_576_T81_Fast(int16_t cs, int16_t dc, int16_t rst, int16_t busy);
    // methods (virtual)
    void clearScreen(uint8_t value = 0xFF);
    void writeScreenBuffer(uint8_t value = 0xFF);
    void writeScreenBufferAgain(uint8_t value = 0xFF);
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
    void writeImageAgain(const uint8_t bitmap[], int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false);
    void writeImagePartAgain(const uint8_t bitmap[], int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
                             int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false);
    void writeNative(const uint8_t* data1, const uint8_t* data2, int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false);
    void drawImage(const uint8_t bitmap[], int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false);
    void drawImagePart(const uint8_t bitmap[], int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
                       int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false);
    void drawImage(const uint8_t* black, const uint8_t* color, int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false);
    void drawImagePart(const uint8_t* black, const uint8_t* color, int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
                       int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false);
    void drawNative(const uint8_t* data1, const uint8_t* data2, int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false);
    void refresh(bool partial_update_mode = false);
    void refresh(int16_t x, int16_t y, int16_t w, int16_t h);
    void powerOff();
    void hibernate();
    void setFastRefresh(bool fast);
    bool isFastRefresh() const { return _want_fast; }
    // Forced LUT temperature for the partial waveform, in degrees C (register
    // 0xE6), or -1 for the controller's internal sensor (the default). Forcing
    // a warm temperature picks a shorter partial LUT: the speed reader
    // (src/rsvp.cpp) runs at 40 for ~420 ms flashes with solid black text. A
    // change takes effect at the next init, which this forces.
    void setForcedTemp(int t);
    int forcedTemp() const { return _forced_temp; }
    // Waveform cut for the speed reader, in ms (0 = off, the default). Each
    // partial refresh is stopped this long after it starts by resetting the
    // controller, which halts the drive, then re-initialising it. At a forced
    // 40 C a 60 ms cut still gives solid black words, and a flash costs about
    // 150 ms instead of 367 ms, at the price of a little more ghosting. Full
    // refreshes are never cut.
    void setCutMs(int ms) { _cut_ms = ms < 0 ? 0 : ms; }
    // Frees the previous-frame shadow. There is deliberately no destructor
    // doing this (GxEPD2_BW copies the driver by value, and a copy would free
    // the same block twice); the owner calls it once before the driver goes
    // away, which src/panel.cpp does when the reader switches panels. The
    // next write would allocate a fresh shadow.
    void releaseShadow() { free(_shadow); _shadow = nullptr; }
  private:
    void _writeBuffer_fill(uint8_t value);
    void _writeBuffer_1bpp(const uint8_t bitmap[], bool invert, bool mirror_y, bool pgm);
    void _writeBuffer_interleaved(const uint8_t* old_buf, const uint8_t* new_buf, bool invert, bool mirror_y, bool pgm);
    void _shadowUpdate(const uint8_t* bitmap, bool invert, bool pgm);
    void _shadowFill(uint8_t value);
    bool _ensureShadow();
    void _Init_Common(bool quick_reset = false);
    void _Init_Full();
    void _Init_Part(bool quick_reset = false);
    void _InitDisplay();
    void _PowerOn();
    void _PowerOff();
    void _Update_Full();
    bool _want_fast = false;
    int _forced_temp = -1;
    int _cut_ms = 0;
    uint8_t* _shadow = nullptr;
    static const uint32_t _shadow_size = ((uint32_t(WIDTH) + 7) / 8) * uint32_t(HEIGHT);
};

#endif
