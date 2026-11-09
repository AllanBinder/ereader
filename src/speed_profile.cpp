#include "speed_profile.h"

#include <Arduino.h>
#include <math.h>
#include <string.h>

#include "config.h"

namespace speed {
namespace {

// SSD1683 0x32 data for black/white mode (datasheet Rev 1.0, figure 6-7), in
// the layout tuned on glass in src/bench.cpp (2026-10-27): five LUTs of 6
// groups x 7 bytes in the order LUTC (VCOM), LUTWW, LUTBW, LUTWB, LUTBB, then
// 14 zero bytes, FR at 224 and XON at 225..226. Per group: byte 0 RP, bytes
// 1..4 phases A..D as VS << 6 | TP, byte 5 SR AB, byte 6 SR CD. With the RED
// RAM holding the previous frame: LUTBW = white to black, LUTWB = black to
// white, LUTWW / LUTBB = unchanged. Only group 0 is used. The tail voltages
// stay the OTP partial's (the driver preloads it before writing this).
//
// Must outlive the driver's pointer to it: static, never freed.
uint8_t lut[227];

const int LUT_WW = 42, LUT_BW = 84, LUT_WB = 126, LUT_BB = 168;
const int LUT_FR = 224;

}  // namespace

const uint8_t *buildLut(LutKind kind, int frames, int frameRate, bool pol, size_t &len) {
  if (kind == LutKind::OTP) {
    len = 0;
    return nullptr;
  }
  if (frames < 1) frames = 1;
  if (frames > 63) frames = 63;
  memset(lut, 0, sizeof(lut));
  // VS 01 = VSH1, 10 = VSL, in bits 7:6. VSH1 = black is the tuned guess.
  const uint8_t black = pol ? 0x80 : 0x40;
  const uint8_t white = pol ? 0x40 : 0x80;
  const uint8_t f = uint8_t(frames & 0x3F);
  const int base[5] = {0, LUT_WW, LUT_BW, LUT_WB, LUT_BB};
  for (int l = 0; l < 5; l++) {
    uint8_t *g = lut + base[l];
    g[0] = 1;  // RP group 0
    g[5] = 1;  // SR AB
    g[6] = 1;  // SR CD
  }
  // Transitions: the same for every kind.
  lut[LUT_BW + 1] = black | f;
  lut[LUT_WB + 1] = white | f;
  if (kind == LutKind::HOLD) {
    lut[LUT_WW + 1] = white | f;
    lut[LUT_BB + 1] = black | f;
  } else if (kind == LutKind::BALANCED) {
    // Phase A the opposite colour, phase B the own colour, F/2 each (rounded
    // down for both when F is odd), so a held pixel nets zero.
    const uint8_t h = uint8_t((frames / 2) & 0x3F);
    if (h) {
      lut[LUT_WW + 1] = black | h;
      lut[LUT_WW + 2] = white | h;
      lut[LUT_BB + 1] = white | h;
      lut[LUT_BB + 2] = black | h;
    }
  }
  lut[LUT_FR] = uint8_t(frameRate & 0x0F);
  len = sizeof(lut);
  return lut;
}

int panelTempC() {
  // Arduino ESP32 core: starts the S3 internal sensor, reads, stops. NAN when
  // the read fails.
  const float t = temperatureRead();
  if (isnan(t) || t < -40.0f || t > 125.0f) return INT16_MIN;
  return (int)lroundf(t) - SPEED_TEMP_OFFSET_C;
}

int framesForTemp(int frames, int tempC) {
  if (tempC != INT16_MIN) {
    for (size_t i = 0; i < sizeof(SPEED_TEMP_STEPS) / sizeof(SPEED_TEMP_STEPS[0]); i++) {
      if (tempC < SPEED_TEMP_STEPS[i].belowC) {
        frames += SPEED_TEMP_STEPS[i].frames;
        break;
      }
    }
  }
  if (frames < 1) frames = 1;
  if (frames > 63) frames = 63;
  return frames;
}

const char *lutKindName(LutKind k) {
  switch (k) {
    case LutKind::OTP: return "OTP";
    case LutKind::NOHOLD: return "NOHOLD";
    case LutKind::HOLD: return "HOLD";
    case LutKind::BALANCED: return "BALANCED";
  }
  return "?";
}

}  // namespace speed
