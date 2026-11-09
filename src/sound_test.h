#pragma once

#include <Arduino.h>

// Sound test for the headphones expansion module (PCM5102A on I2S, pins.h
// PIN_I2S_*), so the module's check needs no bench firmware: the tone sweep
// from src/dac_tone_test.cpp, played once. 440 Hz on both channels for 1 s,
// 0.5 s of silence, then 880 Hz LEFT only for 1 s, then 880 Hz RIGHT only for
// 1 s. 44.1 kHz, 16 bit stereo, about -12 dBFS.
//
// The I2S driver is installed for the sweep and released afterwards, with the
// three pins put back to plain GPIO, so nothing in the reader keeps the
// peripheral. With no DAC on the pins the sweep plays to nobody and the call
// returns as usual.

namespace sound_test {

// True when this build has the DAC's pins (the wide reader). The menu item
// only exists when this is true.
bool fitted();

// Blocking, about 3.5 s. Returns false when the I2S driver would not start.
bool run();

}  // namespace sound_test
