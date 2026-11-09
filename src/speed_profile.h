#pragma once

// Speed read drive profiles and the custom SSD1683 partial waveforms behind
// them (GDEY0579T93, panel 2, fast driver src/drivers/GxEPD2_579_Fast).
//
// A panel row in src/panel.cpp may carry two profiles, Normal (safe, the
// default) and Turbo (custom LUT, faster, more wear); ui::setSpeedDrive(true)
// applies the active one and then any bench console override on top
// (src/ui.cpp). Panels without profiles (0 and 1) keep their own speed
// fields and behaviour.

#include <stddef.h>
#include <stdint.h>

namespace speed {

enum class Mode : uint8_t { NORMAL = 0, TURBO = 1 };

// Partial waveform kinds. OTP is the panel's own partial waveform; the other
// three are built by buildLut below.
//   NOHOLD   changed pixels driven F frames, unchanged pixels not driven.
//            A pixel's flips alternate +F / -F, so it stays DC balanced.
//   HOLD     NOHOLD plus unchanged pixels driven toward their own colour
//            every flash (the 2026-10-27 bench look, no flicker). NOT DC
//            balanced: held black gets the black level every flash and held
//            white the white level. Bench only.
//   BALANCED transitions as NOHOLD; unchanged pixels get the opposite colour
//            for F/2 frames, then their own colour for F/2 (net zero DC).
// NOHOLD and BALANCED read the previous frame (RED RAM), so skip previous is
// only valid under HOLD; ui.cpp gates a profile's skipPrevious on that.
enum class LutKind : uint8_t { OTP = 0, NOHOLD = 1, HOLD = 2, BALANCED = 3 };

struct Profile {
  const char *name;        // "Normal" / "Turbo"
  LutKind lut;             // OTP = panel's own partial waveform
  uint8_t frames;          // custom LUT frames at 20..30 C (before temp steps)
  uint8_t frameRate;       // SSD1683 FR code (2 = 50 Hz)
  bool keepPower, singleWrite, lean, skipPrevious;
  int8_t forcedTemp;       // forced OTP LUT temperature, -1 = panel sensor
  uint16_t wpmMax[4];      // top wpm by words per flash (index 1..3)
  uint8_t fullAfter;       // ghost clearing full at a sentence end after this many partials
  uint8_t fullMax;         // and unconditionally after this many
};

// Fills a static 227 byte SSD1683 table; returns nullptr (len 0) for OTP.
// frames is clamped to 1..63, frameRate is the low nibble of the FR byte,
// pol swaps the two source voltages. The table is one static buffer that the
// next call overwrites in place; the driver keeps the pointer (it does not
// copy), so a rebuild must be followed by setCustomLut to reload it.
const uint8_t *buildLut(LutKind kind, int frames, int frameRate, bool pol, size_t &len);

// Temperature estimate for the panel in C (ESP32-S3 temperatureRead() minus
// SPEED_TEMP_OFFSET_C), or INT16_MIN if unavailable.
int panelTempC();

// frames adjusted by the SPEED_TEMP_STEPS table (config.h), clamped 1..63.
// tempC INT16_MIN (unknown) applies no step.
int framesForTemp(int frames, int tempC);

// "OTP", "NOHOLD", "HOLD", "BALANCED".
const char *lutKindName(LutKind k);

}  // namespace speed
