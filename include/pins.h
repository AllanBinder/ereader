#pragma once

// Single source of truth for all GPIO allocation (ESP32-S3, lolin_s3_mini
// board definition).
//
// Pins avoided on purpose:
//   0, 3, 45, 46  strapping pins (boot mode and log level)
//   19, 20        native USB D-/D+
//   33 through 37 claimed by octal PSRAM on N16R8 style boards
//   43, 44        UART0 TX/RX
// Everything below is free on both the N8R2 and the N16R8 bench boards.

#if defined(PINS_WIDE) && PINS_WIDE
// Wide reader (GDEY0579T93) in the v3 min case, re-pinned 2026-10-28 so each
// part owns one stretch of header: DESPI on the right row by the TX/GND
// corner, DAC at the top of the left row, shoulder keys and d-pad below it.
// Board is an S3-N16R8 dual USB-C devkit: 19/20 are the native USB pair, 33
// to 37 belong to the octal PSRAM. Strapping pin 46 carries the right
// shoulder key only (do not hold it through reset); 0, 3 and 45 are unused.
// Selected by -DPINS_WIDE=1 (esp32s3_579).

// E-paper adapter (DESPI-C579) on the right row by the TX/GND corner: its
// header order BUSY, RES, D/C, CS, SCK, SDI runs 1, 2, 42, 41, 40, 39. GND to
// the corner pin beside TX, VCC to 3V3 on the left row. 38 is left spare.
static const int PIN_EINK_BUSY = 1;
static const int PIN_EINK_RST = 2;
static const int PIN_EINK_DC = 42;
static const int PIN_EINK_CS = 41;
static const int PIN_EINK_SCK = 40;
static const int PIN_EINK_MOSI = 39;  // adapter SDI
static const int PIN_EINK_MISO = -1;  // panel is write-only; no MISO wired

// No sensor on this build: 17/18 now carry the microSD module, so the BME
// probe is skipped (-1 = not fitted, the menu shows "sensor n/a").
static const int PIN_BME_SDA = -1;
static const int PIN_BME_SCL = -1;

// microSD module (expansion, optional) on its own SPI bus (SPI3/HSPI; the
// panel has SPI2), on the left row run right below the DAC. Module VCC to
// 3V3, GND to GND. Proposed 2026-11-03, founder to confirm against the
// module's header. No card or no module = no card books, nothing else.
static const int PIN_SD_CS = 15;
static const int PIN_SD_SCK = 16;
static const int PIN_SD_MOSI = 17;
static const int PIN_SD_MISO = 18;

// D-pad (red 5-way breakout, + U C L D R -) at the bottom of the left row,
// wired out of header order: R 10, D 11, L 12, C 13, U 14. - to GND, + MUST
// go to 3V3 (left open, the module ghosts presses across lines). The module
// sits on the BACK plate rotated 180 and is pressed from behind, so UP and
// DOWN are swapped here; LEFT/RIGHT stay as wired here and are swapped in
// input.cpp instead (PAD_SWAP_LR below), so PAD_LEFT_OFF still names the
// module's L line.
static const int PIN_BTN_UP = 11;      // module D
static const int PIN_BTN_CENTER = 13;
static const int PIN_BTN_LEFT = 12;
static const int PIN_BTN_DOWN = 14;    // module U
static const int PIN_BTN_RIGHT = 10;
#if defined(FOUNDER_UNIT) && FOUNDER_UNIT
// Founder's v4 build only (-DFOUNDER_UNIT=1, env esp32s3_579_founder). A
// reader wired as the guide documents builds without these.
// v4 build (2026-10-31): the module's L line cross-talks with C, so L is not
// read (reader and sequencer); fix in the v5 case. Pin kept for the picker's
// idle-level vote, which only reads it.
static const bool PAD_LEFT_OFF = true;
// v4 build (2026-10-31): the pad reads turned 180 against the picture at the
// default rotation, so the reader turns it back (input.cpp logicalOf). The
// menu's Flip screen still turns it with the picture on top of this.
static const bool PAD_FLIP = true;
#else
static const bool PAD_LEFT_OFF = false;
static const bool PAD_FLIP = false;
#endif
// The pad's LEFT and RIGHT read the other way round from the wiring above,
// every wide build (founder, 2026-11-06). Applied in input.cpp logicalOf to
// the five pad lines only, before any turn.
static const bool PAD_SWAP_LR = true;

// Two top-edge shoulder keys (final min build, 2026-11-06), each a bare tact
// to GND: K1 the left key, K2 the right key as seen from the front. 3 and 8
// carried K3/K4 on the four key builds and are no longer wired. Each key
// acts as one d-pad button, reversed (the right key is LEFT), see
// KEY_ACTS_AS in input.cpp.
static const int PIN_BTN_K1 = 9;   // left key
static const int PIN_BTN_K2 = 46;  // right key


// PCM5102A I2S DAC module at the top of the left row, in module header order
// (VIN, GND, LCK, DIN, BCK, SCK): VIN to the 3V3 pin above, LCK 4, DIN 5,
// BCK 6. Module SCK is tied to GND so the DAC's PLL runs from BCK; no MCLK
// line. 7 is left spare (free for XSMT if boot pops need a mute line).
static const int PIN_I2S_LCK = 4;  // module LCK (word select)
static const int PIN_I2S_DIN = 5;
static const int PIN_I2S_BCK = 6;

#else

// E-paper: Good Display GDEH0576T81 (5.76 inch, 920x680, 1 bit) on a DESPI-C02
// adapter. Same SPI assignment as the Muon firmware and the shelf_display
// project, so this is a known-good map. The adapter labels its header BUSY,
// RES, D/C, CS, SCK, SDI, GND, VCC; set its RESE switch to 0.47.
// Re-cabled 2026-09-09 on the second N8R2 board (header pin 15 fell out, so
// nothing lives on 15). Allocated from the 17 end of the right-hand header
// row; 11, 12, 13 are spare, 3 is left alone as a strapping pin.
static const int PIN_EINK_MOSI = 17;  // adapter SDI
static const int PIN_EINK_SCK = 18;   // adapter SCK
static const int PIN_EINK_CS = 8;     // adapter CS
static const int PIN_EINK_DC = 9;     // adapter D/C
static const int PIN_EINK_RST = 10;   // adapter RES
static const int PIN_EINK_BUSY = 46;  // adapter BUSY (strapping pin; idles LOW at reset so download mode still works)
static const int PIN_EINK_MISO = -1;  // panel is write-only; no MISO wired

// The DESPI-C02 has no panel power enable line: VCC to 3V3 and the panel is
// live, so there is nothing to drive in setup().

// I2C, BME280. No sensor is fitted in the cased build (2026-09-08); the
// firmware probes and shows "n/a" when nothing answers. Pins kept so the
// probe has somewhere harmless to point (12/13 are spare on this build).
static const int PIN_BME_SDA = 12;
static const int PIN_BME_SCL = 13;

// Buttons to GND, INPUT_PULLUP, active LOW.
// Names are physical directions with the device held in portrait. The d-pad
// module is mounted rotated 90 degrees, so its silkscreen labels do not match:
// module RIGHT points up, LEFT points down, UP points left, DOWN points right.
// Moved to the far side of the module 2026-09-08 after the GPIO 15 trace was
// lifted and 5/6 were found bridged. GPIO 15 is dead on this board; never
// assign it. Header pins RX (44) and TX (43) are also free (UART0 is unused,
// logging goes over native USB CDC) and can stand in if reach is easier.
static const int PIN_BTN_UP = 40;     // module RIGHT
static const int PIN_BTN_DOWN = 41;   // module LEFT
static const int PIN_BTN_CENTER = 42;
static const int PIN_BTN_LEFT = 1;    // module UP
static const int PIN_BTN_RIGHT = 2;   // module DOWN

static const bool PAD_LEFT_OFF = false;
static const bool PAD_FLIP = false;
static const bool PAD_SWAP_LR = false;

// No shoulder keys on the cased 5.76 reader.
static const int PIN_BTN_K1 = -1;
static const int PIN_BTN_K2 = -1;

// No expansion modules on the 5.76 reader: no microSD slot and no I2S DAC,
// so the card books and the Sound test menu item are left out (-1 = absent).
static const int PIN_SD_CS = -1;
static const int PIN_SD_SCK = -1;
static const int PIN_SD_MOSI = -1;
static const int PIN_SD_MISO = -1;
static const int PIN_I2S_LCK = -1;
static const int PIN_I2S_DIN = -1;
static const int PIN_I2S_BCK = -1;

#endif  // PINS_WIDE
