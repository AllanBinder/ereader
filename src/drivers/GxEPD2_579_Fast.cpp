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
// GxEPD2_579_Fast: copy of the stock GxEPD2_579_GDEY0579T93 (library 1.6.9)
// with the speed read hooks declared in GxEPD2_579_Fast.h. Changes from the
// stock code are confined to: writeImage / writeImagePart (drive all),
// writeImageAgain / writeImagePartAgain (single write), _InitDisplay and
// _Update_Full (custom LUT bookkeeping), _Update_Part (forced temperature,
// keep power, custom LUT), _writeScreenBuffer / _writeDataFromImage (bulk
// SPI, same bytes in fewer SPI calls), the lean timing branches (delays and
// the busy wait, setLeanTiming), the phase profiling (Profile, micros() only,
// no SPI traffic), and the hook setters at the end of the file. With every
// hook off the SPI command stream and the waits are the stock driver's.
//
// SSD1683 0x22 (Display Update Control 2) bits, as used below:
//   0x80 enable clock      0x40 enable analog     0x20 load temperature
//   0x10 load LUT (OTP)    0x08 display mode 2    0x04 display
//   0x02 disable analog    0x01 disable clock
// so the stock partial 0xFF = power up, load temp + LUT, mode 2 display,
// power down; the stock fast full 0xD7 = the same minus the temperature load
// (0x1A forced to 100 C) and in mode 1.

#include "GxEPD2_579_Fast.h"
#include <string.h>

GxEPD2_579_Fast::GxEPD2_579_Fast(int16_t cs, int16_t dc, int16_t rst, int16_t busy) :
  GxEPD2_EPD(cs, dc, rst, busy, HIGH, 10000000, WIDTH, HEIGHT, panel, hasColor, hasPartialUpdate, hasFastPartialUpdate)
{
}

void GxEPD2_579_Fast::clearScreen(uint8_t value)
{
  // full refresh needed for all cases (previous != screen)
  _writeScreenBuffer(0x26, value); // set previous
  _writeScreenBuffer(0x24, value); // set current
  refresh(false); // full refresh
  _initial_write = false;
}

void GxEPD2_579_Fast::writeScreenBuffer(uint8_t value)
{
  if (_initial_write) return clearScreen(value);
  _writeScreenBuffer(0x24, value); // set current
}

void GxEPD2_579_Fast::writeScreenBufferAgain(uint8_t value)
{
  _writeScreenBuffer(0x24, value); // set current
  _writeScreenBuffer(0x26, value); // set previous
}

void GxEPD2_579_Fast::_writeScreenBuffer(uint8_t command, uint8_t value)
{
  if (!_init_display_done) _InitDisplay();
  _setPartialRamAreaMaster(0, 0, WIDTH / 2, HEIGHT);
  _writeCommand(command);
  _startTransfer();
  _transferFill(value, uint32_t(WIDTH / 2) * uint32_t(HEIGHT) / 8);
  _endTransfer();
  _setPartialRamAreaMaster(0, HEIGHT / 2, WIDTH / 2, HEIGHT);
  _writeCommand(command);
  _startTransfer();
  _transferFill(value, uint32_t(WIDTH / 2) * uint32_t(HEIGHT) / 8);
  _endTransfer();
  _setPartialRamAreaSlave(0, 0, WIDTH / 2, HEIGHT / 2);
  _writeCommand(command | 0x80);
  _startTransfer();
  _transferFill(value, uint32_t(WIDTH / 2) * uint32_t(HEIGHT) / 8);
  _endTransfer();
  _setPartialRamAreaSlave(0, HEIGHT / 2, WIDTH / 2, HEIGHT);
  _writeCommand(command | 0x80);
  _startTransfer();
  _transferFill(value, uint32_t(WIDTH / 2) * uint32_t(HEIGHT) / 8);
  _endTransfer();
}

void GxEPD2_579_Fast::writeImage(const uint8_t bitmap[], int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  // Drive all (setDriveAll): previous = inverse of the new frame, so the
  // mode 2 compare sees every pixel as changed and drives it. writeImageAgain
  // after the refresh sets previous back to the frame itself.
  _profBegin();
  // Previous RAM left stale by skip previous and the OTP LUT back in charge:
  // drive this one frame like drive all, so the compare cannot miss a pixel.
  // Also when a custom LUT that reads the previous frame (skip previous off:
  // NOHOLD, BALANCED) replaced a skipping HOLD table mid run.
  const bool repair = _prev_stale && (!_lut || !_skip_prev);
  const bool all = _drive_all || repair;
  if (all) _writeImage(0x26, bitmap, x, y, w, h, !invert, mirror_y, pgm);
  if (repair) _prev_stale = false;
  _writeImage(0x24, bitmap, x, y, w, h, invert, mirror_y, pgm);
}

void GxEPD2_579_Fast::writeImageForFullRefresh(const uint8_t bitmap[], int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  _profBegin();
  _writeImage(0x26, bitmap, x, y, w, h, invert, mirror_y, pgm); // set previous
  _writeImage(0x24, bitmap, x, y, w, h, invert, mirror_y, pgm); // set current
  _prev_stale = false; // both RAMs now hold this frame
}


// Single write (setSingleWrite): skip the 0x24 half. Safe when current RAM
// (0x24) already holds this exact bitmap for this window, which is how
// GxEPD2_BW calls it: writeImage (or writeImageForFullRefresh) with the same
// buffer and rectangle, then refresh, then this; a refresh does not touch
// 0x24. NOT safe if anything resets the controller or clears RAM between the
// two writes (a future waveform cut, a hibernate wake), or if a caller uses
// writeImageAgain on its own to load a frame. The 0x26 (previous) write must
// stay: SSD1683 does not copy current into previous after a mode 2 update
// unless ping-pong is enabled (0x37), which this driver does not touch.
void GxEPD2_579_Fast::writeImageAgain(const uint8_t bitmap[], int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  // Skip previous (setSkipPrevious): with a custom LUT whose four
  // transitions ignore the previous frame (the bench hold table) 0x26 is
  // never read, so it is not written; writeImage repairs it once the OTP LUT
  // is back.
  if (_skip_prev && _lut) _prev_stale = true;
  else _writeImage(0x26, bitmap, x, y, w, h, invert, mirror_y, pgm); // set previous
  if (!_single_write) _writeImage(0x24, bitmap, x, y, w, h, invert, mirror_y, pgm); // set current
  if (_prof_open && _prof_updated) _profEnd();
}

void GxEPD2_579_Fast::writeImageToPrevious(const uint8_t bitmap[], int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  _writeImage(0x26, bitmap, x, y, w, h, invert, mirror_y, pgm);
}

void GxEPD2_579_Fast::_writeImage(uint8_t command, const uint8_t bitmap[], int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  //Serial.print("writeImage("); Serial.print(x); Serial.print(", "); Serial.print(y); Serial.print(", ");
  //Serial.print(w); Serial.print(", "); Serial.print(h); Serial.println(")");
  uint32_t prof_t0 = micros();
  uint32_t prof_s0 = _prof_cur.setupUs;
  uint32_t prof_y0 = _prof_cur.yieldUs;
  _yield1(); // stock: delay(1), yield() to avoid WDT on ESP8266 and ESP32
  int16_t wbb = (w + 7) / 8; // width bytes, bitmaps are padded
  x -= x >= 0 ? x % 8 : -x % 8; // byte boundary
  w = wbb * 8; // byte boundary
  int16_t x1 = x < 0 ? 0 : x; // limit
  int16_t y1 = y < 0 ? 0 : y; // limit
  int16_t w1 = x + w < int16_t(WIDTH) ? w : int16_t(WIDTH) - x; // limit
  int16_t h1 = y + h < int16_t(HEIGHT) ? h : int16_t(HEIGHT) - y; // limit
  int16_t dx1 = x1 - x; // offset into bitmap
  int16_t dy1 = y1 - y; // offset into bitmap
  w1 -= dx1;
  h1 -= dy1;
  if ((w1 <= 0) || (h1 <= 0)) return;
  if (!_init_display_done) _InitDisplay();
  if (_initial_write) writeScreenBuffer(); // initial full screen buffer clean
  _writeFromImage(command, bitmap, x1, y1, w1, h1, wbb, h, dx1, dy1, invert, mirror_y, pgm);
  _yield1(); // stock: delay(1), yield() to avoid WDT on ESP8266 and ESP32
  _profWrite(command, prof_t0, prof_s0, prof_y0);
}

void GxEPD2_579_Fast::_writeFromImage(uint8_t command, const uint8_t bitmap[], int16_t x1, int16_t y1, int16_t w1, int16_t h1,
    int16_t wbb, int16_t hb, int16_t dx, int16_t dy, bool invert, bool mirror_y, bool pgm)
{
  if ((x1 < WIDTH / 2) && (y1 < HEIGHT / 2) && true) // upper left, near connector
  { // memory is on slave controller
    int16_t ws = x1 + w1 <= WIDTH / 2 ? w1 : WIDTH / 2 - x1;
    int16_t hs = y1 + h1 <= HEIGHT / 2 ? h1 : HEIGHT / 2 - y1;
    //Serial.print("upper left ws="); Serial.print(ws); Serial.print(" hs="); Serial.print(hs); Serial.print(" dx="); Serial.print(dx); Serial.print(" dy="); Serial.println(dy);
    _setPartialRamAreaSlave(x1, y1, ws, hs, 0x03);
    _writeCommand(command | 0x80);
    _writeDataFromImage(bitmap, ws, hs, wbb, hb, dx, dy, invert, mirror_y, pgm);
  }

  if (((x1 + w1) > WIDTH / 2) && (y1 < HEIGHT / 2) && true) // upper right, opposite to connector
  { // memory is on master controller
    int16_t wm = x1 >= WIDTH / 2 ? w1 : (x1 + w1) - WIDTH / 2;
    int16_t hm = y1 + h1 <= HEIGHT / 2 ? h1 : HEIGHT / 2 - y1;
    int16_t xm = x1 < WIDTH / 2 ? WIDTH / 2 - wm : WIDTH - x1 - wm;
    //Serial.print("upper right wm="); Serial.print(wm); Serial.print(" hm="); Serial.print(hm); Serial.print(" dx="); Serial.print(dx); Serial.print(" dy="); Serial.println(dy);
    _setPartialRamAreaMaster(xm, y1, wm, hm, 0x02);
    _writeCommand(command);
    _writeDataFromImage(bitmap, wm, hm, wbb, hb, dx + w1 - wm, dy, invert, mirror_y, pgm);
  }

  if ((x1 < WIDTH / 2) && ((y1 + h1) > HEIGHT / 2) && true) // lower left, near connector
  { // memory is on slave controller
    int16_t ws = x1 + w1 <= WIDTH / 2 ? w1 : WIDTH / 2 - x1;
    int16_t hs = y1 >= HEIGHT / 2 ? h1 : (y1 + h1) - HEIGHT / 2;
    int16_t ys = y1 >= HEIGHT / 2 ? y1 : HEIGHT / 2;
    //Serial.print("lower left ws="); Serial.print(ws); Serial.print(" hs="); Serial.print(hs); Serial.print(" dx="); Serial.print(dx); Serial.print(" dy="); Serial.println(dy);
    _setPartialRamAreaSlave(x1, ys, ws, hs, 0x03);
    _writeCommand(command | 0x80);
    _writeDataFromImage(bitmap, ws, hs, wbb, hb, dx, dy + h1 - hs, invert, mirror_y, pgm);
  }

  if (((x1 + w1) > WIDTH / 2) && ((y1 + h1) > HEIGHT / 2) && true) // lower right, opposite to connector
  { // memory is on master controller
    int16_t wm = x1 >= WIDTH / 2 ? w1 : (x1 + w1) - WIDTH / 2;
    int16_t hm = y1 >= HEIGHT / 2 ? h1 : (y1 + h1) - HEIGHT / 2;
    int16_t xm = x1 < WIDTH / 2 ? WIDTH / 2 - wm : WIDTH - x1 - wm;
    int16_t ym = y1 >= HEIGHT / 2 ? y1 : HEIGHT / 2;
    //Serial.print("lower right wm="); Serial.print(wm); Serial.print(" hm="); Serial.print(hm); Serial.print(" dx="); Serial.print(dx); Serial.print(" dy="); Serial.println(dy);
    _setPartialRamAreaMaster(xm, ym, wm, hm, 0x02);
    _writeCommand(command);
    _writeDataFromImage(bitmap, wm, hm, wbb, hb, dx + w1 - wm, dy + h1 - hm, invert, mirror_y, pgm);
  }
  _yield1(); // stock: delay(1), yield() to avoid WDT on ESP8266 and ESP32
}

// Bulk SPI (setBulkSpi, default on in this driver): the stock loops below
// send one byte per SPIClass::transfer call. The transaction and CS are
// already held for the whole block (_startTransfer / _endTransfer), so the
// cost is the per-call overhead of transfer() itself, measured at ~65 ms
// for one 26928 byte frame at a 20 MHz clock. The bulk path gathers the
// same bytes, in the same order, into s_bulk_buf and sends them with
// SPIClass::writeBytes (FIFO fed, TX only) inside the same transaction, CS
// low and DC high exactly as before. Each byte is computed with the stock
// expression (same idx, mirror_y, pgm, invert), so the bytes on the wire
// are identical; only the call pattern changes. Expected: ~11 ms per frame
// at 20 MHz (26928 bytes * 8 bits / 20 MHz), plus a little FIFO overhead.
// VERIFY on glass: frames with bulk on and bulk off look the same in both
// halves, with no shifted or mirrored rows at the x 396 seam.
// Word aligned: spiWriteNL reads the buffer as uint32_t words.
alignas(4) static uint8_t s_bulk_buf[512];

void GxEPD2_579_Fast::_transferFill(uint8_t value, uint32_t n)
{
  if (!_bulk_spi)
  {
    for (uint32_t i = 0; i < n; i++)
    {
      _transfer(value);
    }
    return;
  }
  memset(s_bulk_buf, value, sizeof(s_bulk_buf));
  while (n > 0)
  {
    uint32_t chunk = n < sizeof(s_bulk_buf) ? n : sizeof(s_bulk_buf);
    _pSPIx->writeBytes(s_bulk_buf, chunk);
    n -= chunk;
  }
}

void GxEPD2_579_Fast::_writeDataFromImage(const uint8_t bitmap[], int16_t w, int16_t h, int16_t wbb, int16_t hb, int16_t dx, int16_t dy, bool invert, bool mirror_y, bool pgm)
{
  _prof_cur.bytes += uint32_t(h) * uint32_t((w + 7) / 8);
  _startTransfer();
  if (_bulk_spi && _lean && !pgm && dx >= 0)
  {
    // Lean timing, row copy: for j = 0, 8, 16, ... the stock index
    // (j + dx) / 8 + row * wbb equals dx / 8 + j / 8 + row * wbb for any
    // dx >= 0 (true for every caller: dx is an offset into the bitmap), so
    // each row is (w + 7) / 8 consecutive bitmap bytes and can be memcpy'd
    // into the SPI buffer. Same bytes, same order; mirror_y only changes
    // which row is read, invert is applied in the buffer. Saves the per byte
    // index math (~1-2 ms per full plane). pgm data takes the stock loop.
    const uint16_t rowBytes = uint16_t((w + 7) / 8);
    uint32_t fill = 0;
    for (int16_t i = 0; i < h; i++)
    {
      int32_t row = mirror_y ? (hb - 1 - (i + dy)) : (i + dy);
      const uint8_t *src = bitmap + dx / 8 + row * int32_t(wbb);
      uint16_t left = rowBytes;
      while (left > 0)
      {
        uint32_t room = sizeof(s_bulk_buf) - fill;
        uint32_t c = left < room ? left : room;
        memcpy(s_bulk_buf + fill, src, c);
        if (invert)
        {
          for (uint32_t k = 0; k < c; k++) s_bulk_buf[fill + k] = ~s_bulk_buf[fill + k];
        }
        fill += c;
        src += c;
        left -= c;
        if (fill == sizeof(s_bulk_buf))
        {
          _pSPIx->writeBytes(s_bulk_buf, fill);
          fill = 0;
        }
      }
    }
    if (fill) _pSPIx->writeBytes(s_bulk_buf, fill);
    _endTransfer();
    return;
  }
  if (_bulk_spi)
  {
    uint32_t fill = 0;
    for (int16_t i = 0; i < h; i++)
    {
      for (int16_t j = 0; j < w; j += 8)
      {
        uint8_t data;
        // same index and byte math as the stock loop below
        int16_t idx = mirror_y ? ((j + dx) / 8 + ((hb - 1 - (i + dy))) * wbb) : ((j + dx) / 8 + (i + dy) * wbb);
        if (pgm)
        {
#if defined(__AVR) || defined(ESP8266) || defined(ESP32)
          data = pgm_read_byte(&bitmap[idx]);
#else
          data = bitmap[idx];
#endif
        }
        else
        {
          data = bitmap[idx];
        }
        if (invert) data = ~data;
        s_bulk_buf[fill++] = data;
        if (fill == sizeof(s_bulk_buf))
        {
          _pSPIx->writeBytes(s_bulk_buf, fill);
          fill = 0;
        }
      }
    }
    if (fill) _pSPIx->writeBytes(s_bulk_buf, fill);
    _endTransfer();
    return;
  }
  for (int16_t i = 0; i < h; i++)
  {
    for (int16_t j = 0; j < w; j += 8)
    {
      uint8_t data;
      // use wbb, h of bitmap for index!
      int16_t idx = mirror_y ? ((j + dx) / 8 + ((hb - 1 - (i + dy))) * wbb) : ((j + dx) / 8 + (i + dy) * wbb);
      if (pgm)
      {
#if defined(__AVR) || defined(ESP8266) || defined(ESP32)
        data = pgm_read_byte(&bitmap[idx]);
#else
        data = bitmap[idx];
#endif
      }
      else
      {
        data = bitmap[idx];
      }
      if (invert) data = ~data;
      _transfer(data);
    }
  }
  _endTransfer();
}

void GxEPD2_579_Fast::writeImagePart(const uint8_t bitmap[], int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
    int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  // Drive all: see writeImage.
  _profBegin();
  const bool repair = _prev_stale && (!_lut || !_skip_prev); // see writeImage
  const bool all = _drive_all || repair;
  if (all) _writeImagePart(0x26, bitmap, x_part, y_part, w_bitmap, h_bitmap, x, y, w, h, !invert, mirror_y, pgm);
  if (repair) _prev_stale = false;
  _writeImagePart(0x24, bitmap, x_part, y_part, w_bitmap, h_bitmap, x, y, w, h, invert, mirror_y, pgm);
}

void GxEPD2_579_Fast::writeImagePartAgain(const uint8_t bitmap[], int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
    int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  // Single write: same condition as writeImageAgain.
  if (!_single_write) _writeImagePart(0x24, bitmap, x_part, y_part, w_bitmap, h_bitmap, x, y, w, h, invert, mirror_y, pgm);
  if (_skip_prev && _lut) _prev_stale = true; // see writeImageAgain
  else _writeImagePart(0x26, bitmap, x_part, y_part, w_bitmap, h_bitmap, x, y, w, h, invert, mirror_y, pgm);
  if (_prof_open && _prof_updated) _profEnd();
}

void GxEPD2_579_Fast::writeImagePartToPrevious(const uint8_t bitmap[], int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
    int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  _writeImagePart(0x26, bitmap, x_part, y_part, w_bitmap, h_bitmap, x, y, w, h, invert, mirror_y, pgm);
}

void GxEPD2_579_Fast::_writeImagePart(uint8_t command, const uint8_t bitmap[], int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
    int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  //Serial.print("writeImagePart("); Serial.print(x_part); Serial.print(", "); Serial.print(y_part); Serial.print(", ");
  //Serial.print(w_bitmap); Serial.print(", "); Serial.print(h_bitmap); Serial.print(", ");
  //Serial.print(x); Serial.print(", "); Serial.print(y); Serial.print(", ");
  //Serial.print(w); Serial.print(", "); Serial.print(h); Serial.println(")");
  uint32_t prof_t0 = micros();
  uint32_t prof_s0 = _prof_cur.setupUs;
  uint32_t prof_y0 = _prof_cur.yieldUs;
  _yield1(); // stock: delay(1), yield() to avoid WDT on ESP8266 and ESP32
  if ((w_bitmap < 0) || (h_bitmap < 0) || (w < 0) || (h < 0)) return;
  if ((x_part < 0) || (x_part >= w_bitmap)) return;
  if ((y_part < 0) || (y_part >= h_bitmap)) return;
  int16_t wb_bitmap = (w_bitmap + 7) / 8; // width bytes, bitmaps are padded
  x_part -= x_part % 8; // byte boundary
  w = w_bitmap - x_part < w ? w_bitmap - x_part : w; // limit
  h = h_bitmap - y_part < h ? h_bitmap - y_part : h; // limit
  x -= x % 8; // byte boundary
  w = 8 * ((w + 7) / 8); // byte boundary, bitmaps are padded
  int16_t x1 = x < 0 ? 0 : x; // limit
  int16_t y1 = y < 0 ? 0 : y; // limit
  int16_t w1 = x + w < int16_t(WIDTH) ? w : int16_t(WIDTH) - x; // limit
  int16_t h1 = y + h < int16_t(HEIGHT) ? h : int16_t(HEIGHT) - y; // limit
  int16_t dx = x1 - x;
  int16_t dy = y1 - y;
  w1 -= dx;
  h1 -= dy;
  if ((w1 <= 0) || (h1 <= 0)) return;
  if (!_init_display_done) _InitDisplay();
  if (_initial_write) writeScreenBuffer(); // initial full screen buffer clean
  _writeFromImage(command, bitmap, x1, y1, w1, h1, wb_bitmap, h_bitmap, dx + x_part, dy + y_part, invert, mirror_y, pgm);
  _yield1(); // stock: delay(1), yield() to avoid WDT on ESP8266 and ESP32
  _profWrite(command, prof_t0, prof_s0, prof_y0);
}

void GxEPD2_579_Fast::writeImage(const uint8_t* black, const uint8_t* color, int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  if (black)
  {
    writeImage(black, x, y, w, h, invert, mirror_y, pgm);
  }
}

void GxEPD2_579_Fast::writeImagePart(const uint8_t* black, const uint8_t* color, int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
    int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  if (black)
  {
    writeImagePart(black, x_part, y_part, w_bitmap, h_bitmap, x, y, w, h, invert, mirror_y, pgm);
  }
}

void GxEPD2_579_Fast::writeNative(const uint8_t* data1, const uint8_t* data2, int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  if (data1)
  {
    writeImage(data1, x, y, w, h, invert, mirror_y, pgm);
  }
}

void GxEPD2_579_Fast::drawImage(const uint8_t bitmap[], int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  writeImage(bitmap, x, y, w, h, invert, mirror_y, pgm);
  refresh(x, y, w, h);
  writeImageAgain(bitmap, x, y, w, h, invert, mirror_y, pgm);
}

void GxEPD2_579_Fast::drawImagePart(const uint8_t bitmap[], int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
    int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  writeImagePart(bitmap, x_part, y_part, w_bitmap, h_bitmap, x, y, w, h, invert, mirror_y, pgm);
  refresh(x, y, w, h);
  writeImagePartAgain(bitmap, x_part, y_part, w_bitmap, h_bitmap, x, y, w, h, invert, mirror_y, pgm);
}

void GxEPD2_579_Fast::drawImage(const uint8_t* black, const uint8_t* color, int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  if (black)
  {
    drawImage(black, x, y, w, h, invert, mirror_y, pgm);
  }
}

void GxEPD2_579_Fast::drawImagePart(const uint8_t* black, const uint8_t* color, int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
    int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  if (black)
  {
    drawImagePart(black, x_part, y_part, w_bitmap, h_bitmap, x, y, w, h, invert, mirror_y, pgm);
  }
}

void GxEPD2_579_Fast::drawNative(const uint8_t* data1, const uint8_t* data2, int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  if (data1)
  {
    drawImage(data1, x, y, w, h, invert, mirror_y, pgm);
  }
}

void GxEPD2_579_Fast::refresh(bool partial_update_mode)
{
  if (partial_update_mode) refresh(0, 0, WIDTH, HEIGHT);
  else
  {
    _setPartialRamAreaSlave(0, 0, WIDTH / 2, HEIGHT);
    _setPartialRamAreaMaster(0, 0, WIDTH / 2, HEIGHT);
    _Update_Full();
    _initial_refresh = false; // initial full update done
  }
}

void GxEPD2_579_Fast::refresh(int16_t x, int16_t y, int16_t w, int16_t h)
{
  if (_initial_refresh) return refresh(false); // initial update needs be full update
  // intersection with screen
  int16_t w1 = x < 0 ? w + x : w; // reduce
  int16_t h1 = y < 0 ? h + y : h; // reduce
  int16_t x1 = x < 0 ? 0 : x; // limit
  int16_t y1 = y < 0 ? 0 : y; // limit
  w1 = x1 + w1 < int16_t(WIDTH) ? w1 : int16_t(WIDTH) - x1; // limit
  h1 = y1 + h1 < int16_t(HEIGHT) ? h1 : int16_t(HEIGHT) - y1; // limit
  if ((w1 <= 0) || (h1 <= 0)) return;
  // make x1, w1 multiple of 8
  w1 += x1 % 8;
  if (w1 % 8 > 0) w1 += 8 - w1 % 8;
  x1 -= x1 % 8;
  _setPartialRamAreaSlave(0, 0, WIDTH / 2, HEIGHT);
  _setPartialRamAreaMaster(0, 0, WIDTH / 2, HEIGHT);
  _Update_Part();
}

void GxEPD2_579_Fast::powerOff()
{
  _PowerOff();
}

void GxEPD2_579_Fast::hibernate()
{
  _PowerOff();
  if (_rst >= 0)
  {
    _writeCommand(0x10); // deep sleep mode
    _writeData(0x1);     // enter deep sleep
    _hibernating = true;
    _init_display_done = false;
  }
}

void GxEPD2_579_Fast::_setPartialRamAreaMaster(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t mode)
{
  //Serial.print("_setPartialRamAreaMaster("); Serial.print(x); Serial.print(", "); Serial.print(y); Serial.print(", ");
  //Serial.print(w); Serial.print(", "); Serial.print(h); Serial.println(")");
  _setPartialRamArea(x, y, w, h, mode, 0x00);
}

void GxEPD2_579_Fast::_setPartialRamAreaSlave(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t mode)
{
  //Serial.print("_setPartialRamAreaSlave("); Serial.print(x); Serial.print(", "); Serial.print(y); Serial.print(", ");
  //Serial.print(w); Serial.print(", "); Serial.print(h); Serial.println(")");
  _setPartialRamArea(x, y, w, h, mode, 0x80);
}

void GxEPD2_579_Fast::_setPartialRamArea(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t mode, uint8_t target)
{
  uint32_t prof_t0 = micros();
  _writeCommand(0x11 | target); // set ram entry mode
  _writeData(mode);
  switch (mode)
  {
    case 0x00: // x decrement, y decrement
      _writeCommand(0x44 | target);
      _writeData((x + w - 1) / 8);
      _writeData(x / 8);
      _writeCommand(0x45 | target);
      _writeData((y + h - 1) % 256);
      _writeData((y + h - 1) / 256);
      _writeData(y % 256);
      _writeData(y / 256);
      _writeCommand(0x4e | target);
      _writeData((x + w - 1) / 8);
      _writeCommand(0x4f | target);
      _writeData((y + h - 1) % 256);
      _writeData((y + h - 1) / 256);
      break;
    case 0x01: // x increment, y decrement
      _writeCommand(0x44 | target);
      _writeData(x / 8);
      _writeData((x + w - 1) / 8);
      _writeCommand(0x45 | target);
      _writeData((y + h - 1) % 256);
      _writeData((y + h - 1) / 256);
      _writeData(y % 256);
      _writeData(y / 256);
      _writeCommand(0x4e | target);
      _writeData(x / 8);
      _writeCommand(0x4f | target);
      _writeData((y + h - 1) % 256);
      _writeData((y + h - 1) / 256);
      break;
    case 0x02: // x decrement, y increment
      _writeCommand(0x44 | target);
      _writeData((x + w - 1) / 8);
      _writeData(x / 8);
      _writeCommand(0x45 | target);
      _writeData(y % 256);
      _writeData(y / 256);
      _writeData((y + h - 1) % 256);
      _writeData((y + h - 1) / 256);
      _writeCommand(0x4e | target);
      _writeData((x + w - 1) / 8);
      _writeCommand(0x4f | target);
      _writeData(y % 256);
      _writeData(y / 256);
      break;
    case 0x03: // x increment, y increment
      _writeCommand(0x44 | target);
      _writeData(x / 8);
      _writeData((x + w - 1) / 8);
      _writeCommand(0x45 | target);
      _writeData(y % 256);
      _writeData(y / 256);
      _writeData((y + h - 1) % 256);
      _writeData((y + h - 1) / 256);
      _writeCommand(0x4e | target);
      _writeData(x / 8);
      _writeCommand(0x4f | target);
      _writeData(y % 256);
      _writeData(y / 256);
      break;
  }
  // Lean timing: skip the stock delay(2). 0x11, 0x44, 0x45, 0x4E and 0x4F
  // only load address registers; the SSD1683 datasheet gives them no BUSY
  // phase and no settle time (BUSY is only specified for 0x12 SWRESET, 0x20
  // master activation and the OTP/LUT loads), and the very next byte is a
  // RAM command that uses the counters just set. Each delay(2) is 1..2 ms
  // at the 1 kHz tick, 10 per full screen partial. VERIFY on glass: no
  // shifted, torn or offset rows in either half, especially the first rows
  // of each quadrant (y 0 and y 136) and at the x 396 seam.
  if (!_lean) delay(2);
  _prof_cur.setupUs += micros() - prof_t0;
  _prof_cur.setups++;
}

void GxEPD2_579_Fast::_PowerOn()
{
  if (!_power_is_on)
  {
    _writeCommand(0x22);
    _writeData(0xe0);
    _writeCommand(0x20);
    _waitWhileBusy("_PowerOn", power_on_time);
  }
  _power_is_on = true;
}

void GxEPD2_579_Fast::_PowerOff()
{
  if (_power_is_on)
  {
    _writeCommand(0x22);
    _writeData(0x83);
    _writeCommand(0x20);
    _waitWhileBusy("_PowerOff", power_off_time);
  }
  _power_is_on = false;
  _using_partial_mode = false;
}

void GxEPD2_579_Fast::_InitDisplay()
{
  if (_hibernating) _reset();
  _writeCommand(0x12);  //SWRESET
  delay(10); // 10ms according to specs, 4ms measured
  _writeCommand(0x18); // Temperature Sensor Control
  _writeData(0x80);    // Internal temperature sensor
  _writeCommand(0x22); // Display Update Control 2
  //_writeData(0xB1);    // Enable clock signal, Load temperature value, Load LUT (3-color mode), Disable clock signal
  _writeData(0xB1);    // Enable clock signal, Load temperature value, Load LUT (black/white mode), Disable clock signal
  _writeCommand(0x20); // Master Activation
  delay(10); // 4ms measured
  _writeCommand(0x1A); // Write to temperature register
  _writeData(0x64);
  _writeData(0x00);
  _writeCommand(0x22); // Display Update Control 2
  _writeData(0x91);    // Load temperature value, Load LUT (black/white mode), Disable clock signal
  _writeCommand(0x20); // Master Activation
  delay(10); // 4ms measured
  _init_display_done = true;
  _lut_loaded = false; // 0x22 0xB1/0x91 above loaded the OTP LUT
}

void GxEPD2_579_Fast::_Update_Full()
{
  uint32_t prof_t0 = micros();
  _writeCommand(0x21); // Display Update Control
  _writeData(0x40);    // bypass RED as 0
  _writeData(0x10);    // cascade application
  if (useFastFullUpdate)
  {
    _writeCommand(0x1A); // Write to temperature register
    _writeData(0x64);
    _writeData(0x00);
    _writeCommand(0x22); // Display Update Control 2
    _writeData(0xd7);
  }
  else
  {
    _writeCommand(0x22); // Display Update Control 2
    _writeData(0xF7);
  }
  _writeCommand(0x20); // Master Activation
  uint32_t prof_b0 = micros();
  _waitWhileBusy("_Update_Full", full_refresh_time); // stock wait always: full refreshes are rationed, nothing to win
  _prof_cur.cmdUs += prof_b0 - prof_t0;
  _prof_cur.busyUs += micros() - prof_b0;
  _prof_cur.full = true;
  _prof_updated = true;
  _power_is_on = false;
  _lut_loaded = false; // 0xD7/0xF7 load the OTP LUT; full refreshes never use the custom one
}

void GxEPD2_579_Fast::_Update_Part()
{
  // Custom LUT (setCustomLut): load it into the LUT registers once; every OTP
  // load (init, full refresh) clears _lut_loaded so it is written again.
  uint32_t prof_t0 = micros();
  // Keep power (setKeepPower): bring the analog rails up once with the stock
  // _PowerOn (0x22 0xE0, a superset of 0xC0 that also latches the sensor
  // temperature; a forced value below overwrites it). No-op while up.
  uint32_t prof_p0 = micros();
  if (_keep_power) _PowerOn();
  uint32_t prof_p1 = micros();
  // After the power up, so the OTP preload in _writeCustomLut knows whether
  // the clock is already running.
  if (_lut && !_lut_loaded) _writeCustomLut();
  // 0x3C and 0x21 are rewritten on every partial, also with keep power on.
  // Left as is under lean timing on purpose: together they are five single
  // byte SPI calls (~20 us), not worth a state cache that could go stale
  // after a full refresh (which writes 0x21 = 0x40 0x10) or a re-init.
  _writeCommand(0x3C); // Border Waveform Control
  _writeData(0x80);    // VCOM
  _writeCommand(0x21); // Display Update Control
  _writeData(0x00);    // RED normal
  _writeData(0x10);    // cascade application
  // Waveform cut (setCutMs) is deliberately not implemented. The T81 driver
  // stops a partial early by pulsing RST and re-initialising, which works
  // there because one UC81xx owns the whole panel and every frame rewrites
  // all of RAM. Here RST is shared by two SSD1683s whose drive timing runs in
  // cascade off the master: a reset mid-waveform halts both at an arbitrary
  // phase without the analog power down (0x22 bit 0x02) discharging the
  // rails, wipes all four RAM quadrants (so both 0x24 and 0x26 must be
  // rewritten before the next compare), and re-runs the init sequence with
  // its temperature and LUT loads (~25 ms of delays). Any skew between the
  // two controllers stopping would show as a seam at x 396. Needs a bench
  // look with a scope on BUSY before it is attempted.
  uint8_t seq = 0x0C; // display mode 2 + display
  if (!_lut_loaded) seq |= 0x10; // load LUT from OTP (skipped when the custom LUT is in)
  if (_forced_temp >= 0)
  {
    // Forced temperature (setForcedTemp): write the register and leave the
    // load temperature bit clear so the sensor does not overwrite it. Sent to
    // the master only, as the stock fast full refresh does with 0x64.
    // VERIFY on bench: that the slave (left half) follows; if the halves
    // differ, also send 0x9A (0x1A | 0x80) with the same bytes.
    _writeCommand(0x1A); // Write to temperature register
    _writeData(uint8_t(_forced_temp)); // integer degrees C (register bits 11:4)
    _writeData(0x00);                  // fraction (bits 3:0 in the high nibble)
  }
  else if (!_lut_loaded) seq |= 0x20; // load temperature from the sensor
  // Keep power: neither enable (0xC0) nor disable (0x03) bits, so the rails
  // and clock stay up from _PowerOn. VERIFY on bench: that 0x0C/0x1C/0x3C
  // run a full mode 2 update with the clock left enabled by 0xE0, and that
  // BUSY timing drops by the ~400 ms ramp.
  if (!_keep_power) seq |= 0xC3;
  // All hooks off: 0x0C | 0x10 | 0x20 | 0xC3 = 0xFF, the stock value.
  _writeCommand(0x22); // Display Update Control 2
  _writeData(seq);
  _writeCommand(0x20);
  uint32_t prof_b0 = micros();
  uint32_t busy_us;
  if (_lean) busy_us = _leanWaitWhileBusy("_Update_Part", partial_refresh_time); // excludes the diag print
  else
  {
    _waitWhileBusy("_Update_Part", partial_refresh_time); // includes the 1 ms margin and the diag print
    busy_us = micros() - prof_b0;
  }
  _power_is_on = _keep_power;
  _prof_cur.powerUs += prof_p1 - prof_p0;
  _prof_cur.cmdUs += (prof_p0 - prof_t0) + (prof_b0 - prof_p1);
  _prof_cur.busyUs += busy_us;
  _prof_updated = true;
}

// ---- Speed read hooks -------------------------------------------------------

void GxEPD2_579_Fast::setForcedTemp(int t)
{
  if (t < 0) t = -1;
  else if (t > 100) t = 100;
  _forced_temp = t;
}

void GxEPD2_579_Fast::setCutMs(int ms)
{
  _cut_ms = ms < 0 ? 0 : ms;
  if (_cut_ms > 0 && !_cut_logged)
  {
    _cut_logged = true;
    Serial.printf("GxEPD2_579_Fast: cut %d ms requested, not implemented (stub), partials run full length\n", _cut_ms);
  }
}

void GxEPD2_579_Fast::setKeepPower(bool on)
{
  if (on == _keep_power) return;
  _keep_power = on;
  // Leaving keep power: drop the rails now rather than at the next full
  // refresh, so they are never left up by a mode change.
  if (!on && _power_is_on && _init_display_done) _PowerOff();
}

void GxEPD2_579_Fast::setCustomLut(const uint8_t *lut, size_t len)
{
  if (lut && len != 227 && len != 233)
  {
    Serial.printf("GxEPD2_579_Fast: custom LUT rejected, %u bytes (want 227 or 233)\n", unsigned(len));
    lut = nullptr;
  }
  if (!lut) len = 0;
  // No early return on an unchanged pointer: the bench rebuilds one static
  // table in place, so the same pointer can carry new contents.
  _lut = lut;
  _lut_len = len;
  // Back to OTP: the next partial sets the load LUT bit again, which
  // replaces whatever is in the register. New table: written before the
  // next partial.
  _lut_loaded = false;
  if (lut) Serial.printf("GxEPD2_579_Fast: custom LUT set, %u bytes\n", unsigned(len));
}

// Writes _lut to both controllers: plain command = master, | 0x80 = slave,
// the cascade addressing the RAM commands above use.
// VERIFY on bench: that 0xB2 (0x32 | 0x80) reaches the slave's LUT register,
// and the same for the tail commands (0xBF, 0x83, 0x84, 0xAC), which follow
// the same rule but are not used anywhere in the stock driver. Also VERIFY
// that the slave should get its own VCOM (0xAC) rather than following the
// master. Byte layout of the 227 byte table is the SSD1683 datasheet's 0x32
// format (VS / TP / RP groups, frame rate, gate level); none is shipped.
void GxEPD2_579_Fast::_writeCustomLut()
{
  if (!_lut) return;
  if (!_init_display_done) _InitDisplay();
  // Preload the OTP black/white (mode 2) waveform first, so a 227 byte table
  // runs with the stock partial's EOPT, VGH, VSH1/VSH2/VSL and VCOM rather
  // than whatever the last OTP load left (a full refresh loads the 3-color
  // mode set at a forced 100 C). 0xB8 keeps the clock running when keep
  // power has it up; 0xB9 matches the stock sequence otherwise.
  _writeCommand(0x22);
  _writeData(_power_is_on ? 0xB8 : 0xB9); // clock, load temperature, load LUT (black/white)
  _writeCommand(0x20);
  _waitWhileBusy("_writeCustomLut", 20);
  const uint8_t targets[2] = {0x00, 0x80};
  for (uint8_t t : targets)
  {
    _writeCommand(0x32 | t); // Write LUT register
    _startTransfer();
    for (size_t i = 0; i < 227; i++) _transfer(_lut[i]);
    _endTransfer();
    if (_lut_len >= 233)
    {
      _writeCommand(0x3F | t); // End option (EOPT)
      _writeData(_lut[227]);
      _writeCommand(0x03 | t); // Gate driving voltage (VGH)
      _writeData(_lut[228]);
      _writeCommand(0x04 | t); // Source driving voltage (VSH1, VSH2, VSL)
      _writeData(_lut[229]);
      _writeData(_lut[230]);
      _writeData(_lut[231]);
      _writeCommand(0x2C | t); // VCOM register
      _writeData(_lut[232]);
    }
    else if (_vsh1)
    {
      _writeCommand(0x04 | t); // Source driving voltage (VSH1, VSH2, VSL)
      _writeData(_vsh1);
      _writeData(_vsh2);
      _writeData(_vsl);
    }
  }
  _lut_loaded = true;
}

void GxEPD2_579_Fast::setSkipPrevious(bool on)
{
  _skip_prev = on;
}

void GxEPD2_579_Fast::setSourceLevels(uint8_t vsh1, uint8_t vsh2, uint8_t vsl)
{
  _vsh1 = vsh1;
  _vsh2 = vsh2;
  _vsl = vsl;
  _lut_loaded = false; // rewritten with the table before the next partial
}

// ---- Lean timing and phase profiling ---------------------------------------

// The stock delay(1) in the image write paths ("yield() to avoid WDT"). On
// ESP32 Arduino it is vTaskDelay(1): 0..1 ms at the 1 kHz tick, longer if a
// higher priority task (WiFi, USB) runs meanwhile. Lean timing skips it: a
// full frame write is now ~12 ms of SPI, far under the task WDT timeout, and
// the busy wait that follows feeds the WDT through the busy callback
// (src/ui.cpp feedWatchdogWhileBusy) without ever sleeping, exactly as stock
// does. VERIFY: no task WDT reports on serial during long speed read runs.
void GxEPD2_579_Fast::_yield1()
{
  if (_lean) return;
  uint32_t t0 = micros();
  delay(1);
  _prof_cur.yieldUs += micros() - t0;
}

// Lean replacement for GxEPD2_EPD::_waitWhileBusy. Stock sleeps a blind
// delay(1) ("margin to become active") before polling; at the 1 kHz tick that
// is anywhere from ~0 to 1 ms, so stock already relies on BUSY rising within
// well under a millisecond of 0x20. Here the margin is a spin that ends as
// soon as BUSY reads active, capped at 2 ms, so it is never shorter than
// stock's margin when it matters. The poll itself is the stock loop (busy
// callback, or yield() when there is none, where stock would delay(1)), with
// the same timeout and the same "_Update_Part : us" diag line, printed after
// the measurement. Returns the BUSY time in us (rise spin included).
// VERIFY on bench: busy reads the same ~281 ms as stock, and frames never
// tear (a tear = RAM written while the update still runs = BUSY missed).
uint32_t GxEPD2_579_Fast::_leanWaitWhileBusy(const char *comment, uint16_t busy_time)
{
  if (_busy < 0)
  {
    delay(busy_time);
    return uint32_t(busy_time) * 1000;
  }
  unsigned long start = micros();
  while (digitalRead(_busy) != _busy_level)
  {
    if (micros() - start > 2000) break; // never saw it rise: same as stock past its margin
  }
  while (digitalRead(_busy) == _busy_level)
  {
    if (_busy_callback) _busy_callback(_busy_callback_parameter);
    if (micros() - start > _busy_timeout)
    {
      Serial.println("Busy Timeout!");
      break;
    }
#if defined(ESP8266) || defined(ESP32)
    yield(); // avoid wdt
#endif
  }
  uint32_t elapsed = micros() - start;
#if !defined(DISABLE_DIAGNOSTIC_OUTPUT)
  if (comment && _diag_enabled)
  {
    Serial.print(comment);
    Serial.print(" : ");
    Serial.println(elapsed);
  }
#endif
  return elapsed;
}

// Opens a profiling cycle at the first image write of a frame. A cycle that
// already ran its update but never saw its closing *Again call is closed
// first, so the counters never mix two frames.
void GxEPD2_579_Fast::_profBegin()
{
  if (_prof_open && !_prof_updated) return; // still inside this frame's writes (drive all)
  if (_prof_open) _profEnd();
  uint32_t n = _prof_last.n;
  _prof_cur = Profile();
  _prof_cur.n = n;
  _prof_updated = false;
  _prof_open = true;
  _prof_t0 = micros();
}

void GxEPD2_579_Fast::_profEnd()
{
  Profile &p = _prof_cur;
  p.totalUs = micros() - _prof_t0;
  uint32_t sum = p.setupUs + p.yieldUs + p.write24Us + p.write26Us + p.cmdUs + p.powerUs + p.busyUs;
  p.otherUs = p.totalUs > sum ? p.totalUs - sum : 0;
  p.lean = _lean;
  p.n++;
  _prof_last = p;
  _prof_open = false;
  _prof_updated = false;
  if (_prof_log)
  {
    Serial.printf("579prof n=%lu %c lean=%d tot=%.1f setup=%.1f/%u yld=%.1f w24=%.1f w26=%.1f cmd=%.1f pwr=%.1f busy=%.1f oth=%.1f ms bytes=%lu\n",
                  (unsigned long)p.n, p.full ? 'F' : 'P', p.lean ? 1 : 0, p.totalUs / 1000.0f,
                  p.setupUs / 1000.0f, (unsigned)p.setups, p.yieldUs / 1000.0f,
                  p.write24Us / 1000.0f, p.write26Us / 1000.0f, p.cmdUs / 1000.0f,
                  p.powerUs / 1000.0f, p.busyUs / 1000.0f, p.otherUs / 1000.0f,
                  (unsigned long)p.bytes);
  }
}

// Books one _writeImage / _writeImagePart call under 0x24 or 0x26, minus the
// RAM window setups and yields inside it (those have their own counters).
void GxEPD2_579_Fast::_profWrite(uint8_t command, uint32_t t0, uint32_t setup0, uint32_t yield0)
{
  uint32_t dt = micros() - t0;
  uint32_t inner = (_prof_cur.setupUs - setup0) + (_prof_cur.yieldUs - yield0);
  dt = dt > inner ? dt - inner : 0;
  if ((command & 0x7F) == 0x26) _prof_cur.write26Us += dt;
  else _prof_cur.write24Us += dt;
}
