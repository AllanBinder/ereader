// GxEPD2_576_T81_Fast: Good Display GDEH0576T81, flash-free partial refresh.
// Vendored from Muon_Decider/src/drivers/GxEPD2_576_GDEH0576T81.cpp; see the
// header for why the class is renamed rather than shadowing the stock one.
//
// Key facts (verified empirically on the Muon bench 2026-04-20):
//   - Panel uses UC81xx commands (0x00 PSR, 0x04 POWER ON, 0x10 DTM1, 0x12 DRF).
//   - BUSY is active LOW on this panel (constructor passes LOW as busy_level).
//   - Wire format is 2bpp: each source byte (8 pixels) expands to 2 output bytes.
//
// Differential (partial) refresh strategy:
//   The 2bpp wire format encodes BOTH old and new pixel states in each pixel's
//   2-bit field: [old_value, new_value]. The partial LUT then only drives
//   transitions (01 = black to white, 10 = white to black), leaving unchanged
//   pixels (00, 11) untouched.
//
//   Full refresh: expand1to2 sets old = new, so every pixel runs the full
//   ghost-clearing waveform.
//
//   Partial refresh: bit interleaving merges the shadow buffer (previous frame)
//   with the new bitmap, so only changed pixels are driven. Fast, no flash.
//
//   A ~78 KB shadow buffer, malloc'd lazily on the first write, tracks the
//   previous frame. writeImageForFullRefresh() rewrites it too, so the base
//   frame after a full refresh is always what the next partial interleaves
//   against.
//
// Init sequence from the Good Display vendor sample (A32-GDEH0576T81.rar).

#include "GxEPD2_576_T81_Fast.h"

#include <string.h>

GxEPD2_576_T81_Fast::GxEPD2_576_T81_Fast(int16_t cs, int16_t dc, int16_t rst, int16_t busy) :
  GxEPD2_EPD(cs, dc, rst, busy, LOW, 30000000, WIDTH, HEIGHT, panel, hasColor, hasPartialUpdate, hasFastPartialUpdate)
{
}

// ---- 2bpp encoding helpers --------------------------------------------------

// Expand one 1bpp byte (8 pixels) into two 2bpp bytes for full refresh.
// Each pixel gets [bit, bit]: old equals new, so the full LUT drives all pixels.
static inline void expand1to2(uint8_t in, uint8_t& hi, uint8_t& lo) {
    hi = 0; lo = 0;
    if (in & 0x80) hi |= 0xC0;
    if (in & 0x40) hi |= 0x30;
    if (in & 0x20) hi |= 0x0C;
    if (in & 0x10) hi |= 0x03;
    if (in & 0x08) lo |= 0xC0;
    if (in & 0x04) lo |= 0x30;
    if (in & 0x02) lo |= 0x0C;
    if (in & 0x01) lo |= 0x03;
}

// Interleave old and new bytes for partial/differential refresh.
// Each pixel gets [old_bit, new_bit]. The partial LUT only drives transitions:
//   11 = white to white (no drive)    10 = white to black (drive)
//   01 = black to white (drive)       00 = black to black (no drive)
static inline void interleave1to2(uint8_t old_b, uint8_t new_b, uint8_t& hi, uint8_t& lo) {
    hi = 0; lo = 0;
    if (old_b & 0x80) hi |= 0x80; if (new_b & 0x80) hi |= 0x40;
    if (old_b & 0x40) hi |= 0x20; if (new_b & 0x40) hi |= 0x10;
    if (old_b & 0x20) hi |= 0x08; if (new_b & 0x20) hi |= 0x04;
    if (old_b & 0x10) hi |= 0x02; if (new_b & 0x10) hi |= 0x01;
    if (old_b & 0x08) lo |= 0x80; if (new_b & 0x08) lo |= 0x40;
    if (old_b & 0x04) lo |= 0x20; if (new_b & 0x04) lo |= 0x10;
    if (old_b & 0x02) lo |= 0x08; if (new_b & 0x02) lo |= 0x04;
    if (old_b & 0x01) lo |= 0x02; if (new_b & 0x01) lo |= 0x01;
}

// ---- Shadow buffer ----------------------------------------------------------

bool GxEPD2_576_T81_Fast::_ensureShadow() {
    if (!_shadow) _shadow = (uint8_t*)malloc(_shadow_size);
    return _shadow != nullptr;
}

void GxEPD2_576_T81_Fast::_shadowFill(uint8_t value) {
    if (_ensureShadow()) memset(_shadow, value, _shadow_size);
}

void GxEPD2_576_T81_Fast::_shadowUpdate(const uint8_t* bitmap, bool invert, bool pgm) {
    if (!_ensureShadow()) return;
    for (uint32_t i = 0; i < _shadow_size; i++) {
        uint8_t b;
        if (pgm) {
#if defined(__AVR) || defined(ESP8266) || defined(ESP32)
            b = pgm_read_byte(&bitmap[i]);
#else
            b = bitmap[i];
#endif
        } else {
            b = bitmap[i];
        }
        if (invert) b = ~b;
        _shadow[i] = b;
    }
}

// ---- Public write/draw API --------------------------------------------------

void GxEPD2_576_T81_Fast::clearScreen(uint8_t value) {
    writeScreenBuffer(value);
    refresh(false);
}

void GxEPD2_576_T81_Fast::writeScreenBuffer(uint8_t value) {
    _InitDisplay();
    _writeCommand(0x10);
    _writeBuffer_fill(value);
    _shadowFill(value);
    _initial_write = false;
}

void GxEPD2_576_T81_Fast::writeScreenBufferAgain(uint8_t value) {
    // Single-phase driver: the shadow is the "again" buffer.
    _shadowFill(value);
}

void GxEPD2_576_T81_Fast::writeImage(const uint8_t bitmap[], int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm) {
    (void)x; (void)y; (void)w; (void)h;
    _InitDisplay();
    _writeCommand(0x10);
    if (_using_partial_mode && _shadow) {
        _writeBuffer_interleaved(_shadow, bitmap, invert, mirror_y, pgm);
    } else {
        _writeBuffer_1bpp(bitmap, invert, mirror_y, pgm);
    }
    _shadowUpdate(bitmap, invert, pgm);
    _initial_write = false;
}

void GxEPD2_576_T81_Fast::writeImageForFullRefresh(const uint8_t bitmap[], int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm) {
    (void)x; (void)y; (void)w; (void)h;
    _InitDisplay();
    _writeCommand(0x10);
    _writeBuffer_1bpp(bitmap, invert, mirror_y, pgm);
    _shadowUpdate(bitmap, invert, pgm);
    _initial_write = false;
}

void GxEPD2_576_T81_Fast::writeImageToPrevious(const uint8_t bitmap[], int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm) {
    (void)x; (void)y; (void)w; (void)h;
    _shadowUpdate(bitmap, invert, pgm);
}

void GxEPD2_576_T81_Fast::writeImagePart(const uint8_t bitmap[], int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
                                            int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm) {
    (void)bitmap; (void)x_part; (void)y_part; (void)w_bitmap; (void)h_bitmap;
    (void)x; (void)y; (void)w; (void)h; (void)invert; (void)mirror_y; (void)pgm;
}

void GxEPD2_576_T81_Fast::writeImagePartToPrevious(const uint8_t bitmap[], int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
                                                     int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm) {
    (void)bitmap; (void)x_part; (void)y_part; (void)w_bitmap; (void)h_bitmap;
    (void)x; (void)y; (void)w; (void)h; (void)invert; (void)mirror_y; (void)pgm;
}

void GxEPD2_576_T81_Fast::writeImage(const uint8_t* black, const uint8_t* color, int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm) {
    (void)color;
    writeImage(black, x, y, w, h, invert, mirror_y, pgm);
}

void GxEPD2_576_T81_Fast::writeImagePart(const uint8_t* black, const uint8_t* color, int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
                                            int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm) {
    (void)color;
    writeImagePart(black, x_part, y_part, w_bitmap, h_bitmap, x, y, w, h, invert, mirror_y, pgm);
}

void GxEPD2_576_T81_Fast::writeImageAgain(const uint8_t bitmap[], int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm) {
    (void)bitmap; (void)x; (void)y; (void)w; (void)h; (void)invert; (void)mirror_y; (void)pgm;
    // Shadow buffer serves as the "again" sync, so no second SPI transfer is needed.
}

void GxEPD2_576_T81_Fast::writeImagePartAgain(const uint8_t bitmap[], int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
                                                 int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm) {
    (void)bitmap; (void)x_part; (void)y_part; (void)w_bitmap; (void)h_bitmap;
    (void)x; (void)y; (void)w; (void)h; (void)invert; (void)mirror_y; (void)pgm;
}

void GxEPD2_576_T81_Fast::writeNative(const uint8_t* data1, const uint8_t* data2, int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm) {
    (void)data1; (void)data2; (void)x; (void)y; (void)w; (void)h; (void)invert; (void)mirror_y; (void)pgm;
}

void GxEPD2_576_T81_Fast::drawImage(const uint8_t bitmap[], int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm) {
    writeImage(bitmap, x, y, w, h, invert, mirror_y, pgm);
    refresh(false);
}

void GxEPD2_576_T81_Fast::drawImagePart(const uint8_t bitmap[], int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
                                           int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm) {
    writeImagePart(bitmap, x_part, y_part, w_bitmap, h_bitmap, x, y, w, h, invert, mirror_y, pgm);
    refresh(false);
}

void GxEPD2_576_T81_Fast::drawImage(const uint8_t* black, const uint8_t* color, int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm) {
    writeImage(black, color, x, y, w, h, invert, mirror_y, pgm);
    refresh(false);
}

void GxEPD2_576_T81_Fast::drawImagePart(const uint8_t* black, const uint8_t* color, int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
                                           int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm) {
    writeImagePart(black, color, x_part, y_part, w_bitmap, h_bitmap, x, y, w, h, invert, mirror_y, pgm);
    refresh(false);
}

void GxEPD2_576_T81_Fast::drawNative(const uint8_t* data1, const uint8_t* data2, int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm) {
    writeNative(data1, data2, x, y, w, h, invert, mirror_y, pgm);
    refresh(false);
}

void GxEPD2_576_T81_Fast::refresh(bool partial_update_mode) {
    _Update_Full();
    if (!partial_update_mode) {
        _PowerOff();
    }
    _initial_refresh = false;
}

void GxEPD2_576_T81_Fast::refresh(int16_t x, int16_t y, int16_t w, int16_t h) {
    (void)x; (void)y; (void)w; (void)h;
    _Update_Full();
    _initial_refresh = false;
}

void GxEPD2_576_T81_Fast::powerOff() {
    _PowerOff();
}

void GxEPD2_576_T81_Fast::hibernate() {
    _PowerOff();
    if (_rst >= 0) {
        _writeCommand(0x07);
        _writeData(0xA5);
        _hibernating = true;
        _init_display_done = false;
    }
}

// ---- Private helpers --------------------------------------------------------

void GxEPD2_576_T81_Fast::_writeBuffer_fill(uint8_t value) {
    // Row buffered bulk writes: one SPI burst per panel row instead of one
    // transfer() call per byte, which was the bulk of every refresh.
    _startTransfer();
    uint8_t rowbuf[(WIDTH + 7) / 8 * 2];
    memset(rowbuf, value, sizeof(rowbuf));
    for (int32_t row = 0; row < int32_t(HEIGHT); row++) {
        _pSPIx->writeBytes(rowbuf, sizeof(rowbuf));
    }
    _endTransfer();
}

void GxEPD2_576_T81_Fast::_writeBuffer_1bpp(const uint8_t bitmap[], bool invert, bool mirror_y, bool pgm) {
    _startTransfer();
    int32_t wb = (int32_t(WIDTH) + 7) / 8;
    uint8_t rowbuf[(WIDTH + 7) / 8 * 2];
    for (int32_t row = 0; row < int32_t(HEIGHT); row++) {
        int32_t src_row = mirror_y ? row : (int32_t(HEIGHT) - 1 - row);
        for (int32_t col = 0; col < wb; col++) {
            uint32_t idx = uint32_t(col) + uint32_t(src_row) * uint32_t(wb);
            uint8_t b;
            if (pgm) {
#if defined(__AVR) || defined(ESP8266) || defined(ESP32)
                b = pgm_read_byte(&bitmap[idx]);
#else
                b = bitmap[idx];
#endif
            } else {
                b = bitmap[idx];
            }
            if (invert) b = ~b;
            uint8_t hi, lo;
            expand1to2(b, hi, lo);
            rowbuf[col * 2] = hi;
            rowbuf[col * 2 + 1] = lo;
        }
        _pSPIx->writeBytes(rowbuf, uint32_t(wb) * 2);
    }
    _endTransfer();
}

void GxEPD2_576_T81_Fast::_writeBuffer_interleaved(const uint8_t* old_buf, const uint8_t* new_buf, bool invert, bool mirror_y, bool pgm) {
    _startTransfer();
    int32_t wb = (int32_t(WIDTH) + 7) / 8;
    uint8_t rowbuf[(WIDTH + 7) / 8 * 2];
    for (int32_t row = 0; row < int32_t(HEIGHT); row++) {
        int32_t src_row = mirror_y ? row : (int32_t(HEIGHT) - 1 - row);
        for (int32_t col = 0; col < wb; col++) {
            uint32_t idx = uint32_t(col) + uint32_t(src_row) * uint32_t(wb);
            uint8_t old_b = old_buf[idx];
            uint8_t new_b;
            if (pgm) {
#if defined(__AVR) || defined(ESP8266) || defined(ESP32)
                new_b = pgm_read_byte(&new_buf[idx]);
#else
                new_b = new_buf[idx];
#endif
            } else {
                new_b = new_buf[idx];
            }
            if (invert) new_b = ~new_b;
            if (_drive_all) old_b = ~new_b;  // every pixel a transition, see setDriveAll
            uint8_t hi, lo;
            interleave1to2(old_b, new_b, hi, lo);
            rowbuf[col * 2] = hi;
            rowbuf[col * 2 + 1] = lo;
        }
        _pSPIx->writeBytes(rowbuf, uint32_t(wb) * 2);
    }
    _endTransfer();
}

void GxEPD2_576_T81_Fast::_Init_Common(bool quick_reset) {
    if (quick_reset) {
        // Short pulse for the waveform cut: the stock _reset() spends ~40 ms
        // in delays, most of a cut flash's budget. 200 us low / 500 us high
        // was enough on the first panel batch; the second batch (2026-10)
        // intermittently ran a noisy, heavily ghosting waveform until the
        // next full init at those widths, so 2 ms each.
        digitalWrite(_rst, LOW); delay(2);
        digitalWrite(_rst, HIGH); delay(2);
    } else {
        _reset();
        delay(10);
    }
    _waitWhileBusy("Init reset", 500);

    _writeCommand(0x00);                                              // PSR
    _writeData(0x27); _writeData(0x0E);
    _waitWhileBusy("Init PSR", 500);

    _writeCommand(0x06);                                              // BTST
    _writeData(0x0F); _writeData(0x8B); _writeData(0x9C); _writeData(0xC1);

    _writeCommand(0xE7); _writeData(0xC1);                            // PST
    _writeCommand(0x30); _writeData(0x08);                            // frame rate
    _writeCommand(0x50); _writeData(0x77);                            // CDI, border white

    _writeCommand(0x61);                                              // TRES
    _writeData(WIDTH / 256); _writeData(WIDTH % 256);
    _writeData(HEIGHT / 256); _writeData(HEIGHT % 256);

    _writeCommand(0x62);                                              // HTOTAL
    _writeData(0x98); _writeData(0x98); _writeData(0x98); _writeData(0x75);
    _writeData(0xCA); _writeData(0xB2); _writeData(0x98); _writeData(0x7E);

    _writeCommand(0x65);                                              // GSST
    _writeData(0x00); _writeData(0x00); _writeData(0x00); _writeData(0x00);

    _writeCommand(0xE9); _writeData(0x01);                            // PST
}

void GxEPD2_576_T81_Fast::_Init_Full() {
    _Init_Common();
    _writeCommand(0xE0); _writeData(0x02);
    _writeCommand(0xE6); _writeData(241);
    _writeCommand(0xA5);
    _waitWhileBusy("Init LUT full", 500);
    delay(10);

    _writeCommand(0x04);
    _waitWhileBusy("Init PowerOn", power_on_time);

    _init_display_done = true;
    _power_is_on = true;
    _using_partial_mode = false;
    _hibernating = false;
}

void GxEPD2_576_T81_Fast::_Init_Part(bool quick_reset) {
    _Init_Common(quick_reset);
    if (_forced_temp >= 0) {
        _writeCommand(0xE0); _writeData(0x02);
        _writeCommand(0xE6); _writeData((uint8_t)_forced_temp);
    } else {
        _writeCommand(0xE0); _writeData(0x00);
    }
    _writeCommand(0xA5);
    _waitWhileBusy("Init LUT part", 500);

    _writeCommand(0x04);
    _waitWhileBusy("Init PowerOn", power_on_time);

    _init_display_done = true;
    _power_is_on = true;
    _using_partial_mode = true;
    _hibernating = false;
}

void GxEPD2_576_T81_Fast::_InitDisplay() {
    if (_init_display_done && !_hibernating) return;
    if (_want_fast) _Init_Part();
    else _Init_Full();
}

void GxEPD2_576_T81_Fast::setFastRefresh(bool fast) {
    // Fast mode without a shadow buffer would be worse than slow: the partial
    // LUT only drives pixels whose old and new bits differ, so a frame sent
    // with old = new (which is all _writeBuffer_1bpp can produce) drives
    // nothing at all and the panel keeps the stale image. If the ~78 KB
    // allocation cannot be had, stay on the full waveform.
    if (fast && !_ensureShadow()) fast = false;
    if (fast != _want_fast) {
        _want_fast = fast;
        _init_display_done = false;
    }
}

void GxEPD2_576_T81_Fast::setForcedTemp(int t) {
    if (t < 0) t = -1;
    if (t != _forced_temp) {
        _forced_temp = t;
        _init_display_done = false;
    }
}

void GxEPD2_576_T81_Fast::_PowerOn() {
    if (!_power_is_on) {
        _writeCommand(0x04);
        _waitWhileBusy("_PowerOn", power_on_time);
    }
    _power_is_on = true;
}

void GxEPD2_576_T81_Fast::_PowerOff() {
    if (_power_is_on) {
        _writeCommand(0x02); _writeData(0x00);
        _waitWhileBusy("_PowerOff", power_off_time);
    }
    _power_is_on = false;
    _init_display_done = false;
}

void GxEPD2_576_T81_Fast::_Update_Full() {
    _writeCommand(0x12); _writeData(0x00);
    if (_cut_ms > 0 && _using_partial_mode && _rst >= 0) {
        // Stop the partial waveform early (see setCutMs). The reset leaves
        // the controller's image RAM undefined, which is fine because every
        // frame here rewrites the whole panel, old and new bits both.
        delay(_cut_ms);
        _Init_Part(true);
        return;
    }
    _waitWhileBusy("_Update_Full", full_refresh_time);
}
