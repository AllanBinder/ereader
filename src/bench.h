#pragma once

// Bench console: a line oriented serial console on the USB CDC port for the
// speed read tuning session of panel 2's fast driver (env esp32s3_579).
// Host side: tools/rsvp_bench.py. Type `help` for the commands.
//
// The production drive (keep power, single write, lean timing, LUT, skip
// previous) comes from the panel's speed profile (src/speed_profile.h,
// Normal or Turbo, picked with `mode` or in settings), not from here. Every
// drive setting made at the console is an override on top of the profile
// until `clear` or a reboot; `stat` lists which ones are in force.
//
// Reading is non blocking (Serial.available), so a board with no host
// attached never waits on it. Replies are a line or three, and nothing is
// printed per flash unless `log 1` asked for it: a USB CDC port nobody drains
// can block writes, which has hung this board before.
//
// It is harmless on every build: hook commands answer "no hooks (stock
// driver)" without a driver that has them, mode and ptemp answer "no
// profiles on this panel" without speed profiles, and nothing changes until
// a command arrives.

namespace bench {

// Call once from setup(), after rsvp::begin().
void begin();

// Call from every loop() pass: reads what has arrived and runs complete lines
// (ended by \n or \r).
void poll();

}  // namespace bench
