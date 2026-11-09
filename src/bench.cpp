#include "bench.h"

#include <Arduino.h>
#include <esp_task_wdt.h>
#include <string.h>

#include "rsvp.h"
#include "speed_profile.h"
#include "ui.h"

namespace {

void say(const char *fmt, ...) {
  char buf[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  Serial.print(buf);
}

// One command line. Longer lines are dropped whole (`overflow`).
const int CMD_LINE_MAX = 48;
char line[CMD_LINE_MAX + 1];
int lineLen = 0;
bool overflow = false;

// Help, one say() per line (each under its 160 byte buffer).
const char *const HELP[] = {
    "help | stat | ptemp | mode 0|1 (Normal|Turbo) | clear (drop console overrides)\n",
    "kp 0|1 | sw 0|1 | temp N (-1 sensor) | push | wpm N (100-1000) | words 1|2|3 | full\n",
    "log 0|1 | play | pause | bulk 0|1 | lean 0|1 | prof 0|1 | inc 0|1 | win 0|1\n",
    "blit 0|1 | blitcheck | volt 0|14-17 | np 0|1 | spi MHz (0 spec) | video [F]\n",
    "lut 0 (OTP) | lut F [FR [KIND [POL]]]  KIND 0 NOHOLD, 1 HOLD, 2 BALANCED\n",
    "sclear 0|1 (Turbo ghost clear: black + blank partials instead of a full)\n",
};

// Frame rate code (SSD1683 FR, low nibble) to tenths of a Hz, 0 = invalid.
const int HZ10[13] = {0, 250, 500, 750, 1000, 1250, 0, 0, 0, 375, 625, 875, 1125};
int hz10(int fr) { return (fr >= 0 && fr <= 12) ? HZ10[fr] : 0; }

void video(int lutFrames);  // below, with its helpers

// The console's own record of what it set and ui::BenchDrive cannot report
// back (the LUT's polarity, source levels, SPI clock), so `video F` can put
// the console state back after borrowing a LUT for a run. This file is the
// only caller of those ui setters, and `clear` resets the record.
struct LutSet {
  speed::LutKind kind;  // OTP = `lut 0`
  int frames, fr;
  bool pol;
};
LutSet lutSet = {speed::LutKind::OTP, 0, 0, false};  // valid while BenchDrive.ovLut
uint8_t voltSet = 0;  // `volt` level, 0 = OTP levels
int spiMhzSet = 0;    // `spi` clock, 0 = the panel spec

// Hook commands need a driver that has the hooks (the fast panel 2 build, or
// panel 0's T81 driver, which ignores kp and sw).
bool needHooks() {
  if (ui::speedHooksActive()) return true;
  say("no hooks (stock driver)\n");
  return false;
}

// Speed mode commands need a panel with speed profiles (panel 2).
bool needProfile() {
  if (ui::activeSpeedProfile()) return true;
  say("no profiles on this panel\n");
  return false;
}

// "0" or "1" as a flag; false (and a reply) for anything else.
bool parseFlag(const char *arg, bool &out) {
  if (arg && (!strcmp(arg, "0") || !strcmp(arg, "1"))) {
    out = arg[0] == '1';
    return true;
  }
  say("want 0 or 1\n");
  return false;
}

bool parseInt(const char *arg, int lo, int hi, int &out) {
  char *end = nullptr;
  long v = arg ? strtol(arg, &end, 10) : 0;
  if (!arg || end == arg || *end || v < lo || v > hi) {
    say("want %d..%d\n", lo, hi);
    return false;
  }
  out = (int)v;
  return true;
}

// The LUT speed read runs with, e.g. "NOHOLD F 6 @ 50.0 Hz", "OTP", "raw".
void lutText(const ui::BenchDrive &d, char *out, size_t n) {
  if (d.lutRaw) {
    snprintf(out, n, "raw");
  } else if (d.lutKind == speed::LutKind::OTP) {
    snprintf(out, n, "OTP");
  } else {
    const int h = hz10(d.frameRate);
    snprintf(out, n, "%s F %d @ %d.%d Hz", speed::lutKindName(d.lutKind), d.frames, h / 10,
             h % 10);
  }
}

void tempText(int c, char *out, size_t n) {
  if (c == INT16_MIN) snprintf(out, n, "n/a");
  else snprintf(out, n, "%d C", c);
}

void stat() {
  const ui::BenchDrive d = ui::benchDrive();
  const rsvp::BenchStats s = rsvp::benchStats();
  char lut[40], t[12];
  lutText(d, lut, sizeof(lut));
  tempText(d.tempC, t, sizeof(t));
  if (d.hasProfile) {
    say("mode %s  profile %s  LUT %s  temp at drive on %s\n",
        d.mode == speed::Mode::TURBO ? "turbo" : "normal", d.profile, lut, t);
  } else {
    say("no speed profiles on this panel  LUT %s\n", lut);
  }
  // kp, sw, lean, np and temp are the effective values: the console's
  // override, else the profile's.
  say("hooks %s  drive %s  kp %d  sw %d  lean %d  np %d  temp %d  bulk %d\n",
      ui::speedHooksActive() ? "yes" : "no", d.speedDrive ? "on" : "off", d.keepPower,
      d.singleWrite, d.leanSet, d.skipPrevious, d.temp, d.bulkSpi);
  char ov[80];
  snprintf(ov, sizeof(ov), "%s%s%s%s%s%s%s%s", d.ovKeepPower ? " kp" : "",
           d.ovSingleWrite ? " sw" : "", d.ovLean ? " lean" : "", d.ovSkipPrevious ? " np" : "",
           d.ovTemp ? " temp" : "", d.ovLut ? " lut" : "", voltSet ? " volt" : "",
           spiMhzSet ? " spi" : "");
  say("console overrides:%s%s\n", ov[0] ? ov : " none", ov[0] ? " (clear drops them)" : "");
  say("inc %d  win %d%s  blit %d%s\n", s.incremental, s.window,
      s.windowCapable ? "" : " (no window on this driver)", s.blit,
      s.blitCapable ? "" : " (falls back to GFX here)");
  say("speed read %s  %s  %d wpm  %dw  interval %lu  cut %d  flashes %lu  since full %d%s%s%s\n",
      rsvp::benchActive() ? "on" : "off", rsvp::benchPlaying() ? "playing" : "paused", s.wpm,
      s.words, (unsigned long)s.intervalMs, s.cutMs, (unsigned long)s.flashes,
      s.partialsSinceFull, s.fullPending ? "  full queued" : "",
      s.pushPending ? "  push queued" : "", s.log ? "  log" : "");
  if (!s.count) {
    say("last 0 flashes\n");
    return;
  }
  say("last %d: avg %lu  min %lu  max %lu  gap %lu  late %d\n", s.count,
      (unsigned long)s.avgMs, (unsigned long)s.minMs, (unsigned long)s.maxMs,
      (unsigned long)s.avgGapMs, s.late);
  say("  compose %lu  out %lu  band %d  win %d  full %d  blit %d\n", (unsigned long)s.avgComposeMs,
      (unsigned long)s.avgOutMs, s.bandFlashes, s.windowFlashes,
      s.count - s.bandFlashes - s.windowFlashes, s.blitFlashes);
  // Real rate vs. the set wpm: planned folds in the dwell, achieved also the
  // late frames and ghost clearing fulls.
  say("  rate: achieved %d wpm  planned %d (dwell incl.)  set %d\n", s.achievedWpm,
      s.plannedWpm, s.wpm);
}

// Fresh panel temperature and what the active profile would build from it.
void ptemp() {
  const speed::Profile *p = ui::activeSpeedProfile();
  const int c = speed::panelTempC();
  char t[12];
  tempText(c, t, sizeof(t));
  if (p->lut == speed::LutKind::OTP) {
    say("panel %s  profile %s  LUT OTP (no frames to step)\n", t, p->name);
  } else {
    say("panel %s  profile %s  LUT %s  frames %d (base %d)\n", t, p->name,
        speed::lutKindName(p->lut), speed::framesForTemp(p->frames, c), p->frames);
  }
  if (ui::benchDrive().ovLut) say("  (console lut override in force; clear to use the profile)\n");
}

// Symmetric VSH1 / VSL for the custom LUT, VSH2 at its 5 V POR value (the
// bench LUT never selects VSH2). 0 = the OTP partial's levels, else 14..17 V.
void setVolt(int v) {
  static const uint8_t VSH1[4] = {0x3C, 0x41, 0x46, 0x4B};  // 14, 15, 16, 17 V
  static const uint8_t VSL[4] = {0x2E, 0x32, 0x36, 0x3A};   // -14 .. -17 V
  if (v == 0) ui::benchSetSourceLevels(0, 0, 0);
  else ui::benchSetSourceLevels(VSH1[v - 14], 0xA8, VSL[v - 14]);
}

// Puts the console LUT override back after `video` borrowed one. Without a
// LUT override before the run, ui has no call that drops only the LUT
// override, so this drops all of them and sets the others again from what
// they were (BenchDrive's effective values, plus this file's record).
void restoreLut(const ui::BenchDrive &before) {
  if (before.ovLut) {
    ui::benchSetLut(lutSet.kind, lutSet.frames, lutSet.fr, lutSet.pol);
    return;
  }
  ui::benchClearOverrides();
  if (before.ovKeepPower) ui::benchSetKeepPower(before.keepPower);
  if (before.ovSingleWrite) ui::benchSetSingleWrite(before.singleWrite);
  if (before.ovLean) ui::benchSetLeanTiming(before.leanSet);
  if (before.ovSkipPrevious) ui::benchSetSkipPrevious(before.skipPrevious);
  if (before.ovTemp) ui::benchSetTemp(before.temp);
  if (voltSet) setVolt(voltSet);
  if (spiMhzSet) ui::benchSetSpiHz((uint32_t)spiMhzSet * 1000000u);
}

void run(char *cmd) {
  while (*cmd == ' ' || *cmd == '\t') cmd++;
  for (int i = (int)strlen(cmd) - 1; i >= 0 && (cmd[i] == ' ' || cmd[i] == '\t'); i--) {
    cmd[i] = 0;
  }
  if (!*cmd) return;
  char *arg = strchr(cmd, ' ');
  if (arg) {
    *arg++ = 0;
    while (*arg == ' ') arg++;
    if (!*arg) arg = nullptr;
  }
  bool f;
  int n;
  if (!strcmp(cmd, "help")) {
    for (const char *h : HELP) say("%s", h);
  } else if (!strcmp(cmd, "stat")) {
    stat();
  } else if (!strcmp(cmd, "ptemp")) {
    if (!needProfile()) return;
    ptemp();
  } else if (!strcmp(cmd, "mode")) {
    if (!needProfile() || !parseInt(arg, 0, 1, n)) return;
    rsvp::setMode(n ? speed::Mode::TURBO : speed::Mode::NORMAL);
    rsvp::benchResetStats();
    say("ok mode %d (%s)\n", n, ui::activeSpeedProfile()->name);
  } else if (!strcmp(cmd, "clear")) {
    ui::benchClearOverrides();
    lutSet = {speed::LutKind::OTP, 0, 0, false};
    voltSet = 0;
    spiMhzSet = 0;
    rsvp::benchResetStats();
    say("ok clear (the %s drive is back in charge)\n",
        ui::activeSpeedProfile() ? "profile's" : "fixed");
  } else if (!strcmp(cmd, "kp")) {
    if (!needHooks() || !parseFlag(arg, f)) return;
    ui::benchSetKeepPower(f);
    rsvp::benchResetStats();
    say("ok kp %d\n", f);
  } else if (!strcmp(cmd, "sw")) {
    if (!needHooks() || !parseFlag(arg, f)) return;
    ui::benchSetSingleWrite(f);
    rsvp::benchResetStats();
    say("ok sw %d\n", f);
  } else if (!strcmp(cmd, "temp")) {
    if (!needHooks() || !parseInt(arg, -1, 100, n)) return;
    ui::benchSetTemp(n);
    rsvp::benchResetStats();
    say("ok temp %d\n", n);
  } else if (!strcmp(cmd, "sclear")) {
    if (!parseFlag(arg, f)) return;
    rsvp::benchSetSoftClear(f);
    say("ok sclear %d\n", f);
  } else if (!strcmp(cmd, "push")) {
    if (!needHooks()) return;
    rsvp::benchPushOnce();
    say("ok push next flash\n");
  } else if (!strcmp(cmd, "wpm")) {
    if (!parseInt(arg, 100, 1000, n)) return;
    rsvp::benchSetWpm(n);
    rsvp::benchResetStats();
    say("ok wpm %d\n", n);
  } else if (!strcmp(cmd, "words")) {
    if (!parseInt(arg, 1, 3, n)) return;
    rsvp::benchSetWords(n);
    rsvp::benchResetStats();
    say("ok words %d\n", n);
  } else if (!strcmp(cmd, "full")) {
    if (!rsvp::benchActive()) {
      say("not in speed read\n");
      return;
    }
    rsvp::benchForceFull();
    say("ok full at next due\n");
  } else if (!strcmp(cmd, "log")) {
    if (!parseFlag(arg, f)) return;
    rsvp::benchSetLog(f);
    say("ok log %d\n", f);
  } else if (!strcmp(cmd, "bulk")) {
    if (!needHooks() || !parseFlag(arg, f)) return;
    ui::benchSetBulkSpi(f);
    rsvp::benchResetStats();
    say("ok bulk %d%s\n", f, ui::benchDrive().bulkSpi == f ? "" : " (driver has no bulk SPI)");
  } else if (!strcmp(cmd, "lean")) {
    if (!needHooks() || !parseFlag(arg, f)) return;
    ui::benchSetLeanTiming(f);
    rsvp::benchResetStats();
    say("ok lean %d%s\n", f, ui::benchDrive().leanTiming == f ? "" : " (driver has no lean timing)");
  } else if (!strcmp(cmd, "prof")) {
    if (!needHooks() || !parseFlag(arg, f)) return;
    ui::benchSetProfileLog(f);
    say("ok prof %d\n", f);
  } else if (!strcmp(cmd, "inc")) {
    if (!parseFlag(arg, f)) return;
    rsvp::benchSetIncremental(f);
    rsvp::benchResetStats();
    say("ok inc %d\n", f);
  } else if (!strcmp(cmd, "win")) {
    if (!parseFlag(arg, f)) return;
    rsvp::benchSetWindow(f);
    rsvp::benchResetStats();
    say("ok win %d%s\n", f, (f && !ui::windowFrames()) ? " (ignored: driver ships whole frames)" : "");
  } else if (!strcmp(cmd, "blit")) {
    if (!parseFlag(arg, f)) return;
    rsvp::benchSetBlit(f);
    rsvp::benchResetStats();
    say("ok blit %d%s\n", f, (f && !ui::blitAvailable()) ? " (falls back to GFX here)" : "");
  } else if (!strcmp(cmd, "blitcheck")) {
    const rsvp::BlitCheck c = rsvp::benchBlitCheck();
    if (!c.ran) {
      say("blitcheck: %s\n", c.why);
      return;
    }
    say("blitcheck: %s  mismatch %d  flipped %d  (%u bytes x 2 bg)  fallback %d\n",
        (c.mismatch || c.mismatchFlip || c.fellBack) ? "FAIL" : "ok", c.mismatch,
        c.mismatchFlip, (unsigned)c.bytes, c.fellBack);
    say("  gfx %lu us  blit %lu us\n", (unsigned long)c.gfxUs, (unsigned long)c.blitUs);
  } else if (!strcmp(cmd, "lut")) {
    // lut F [FR [KIND [POL]]]: a console LUT override (frames as given, no
    // temperature steps) that wins over the profile until `clear`. KIND 0
    // and 1 keep the old HOLD flag's meaning (0 NOHOLD, 1 HOLD).
    if (!needHooks()) return;
    int v[4] = {-1, 2, 0, 0};  // frames, FR code, kind, pol
    int got = 0;
    char *p = arg;
    while (p && *p && got < 4) {
      char *end = nullptr;
      long x = strtol(p, &end, 10);
      if (end == p) break;
      v[got++] = (int)x;
      p = end;
      while (*p == ' ') p++;
    }
    if (got == 0 || (p && *p)) {
      say("want lut 0 | lut F [FR [KIND [POL]]]\n");
      return;
    }
    if (v[0] == 0) {
      lutSet = {speed::LutKind::OTP, 0, 0, false};
      ui::benchSetLut(speed::LutKind::OTP, 0, 0, false);
      rsvp::benchResetStats();
      say("ok lut off (OTP override)\n");
      return;
    }
    if (v[0] < 1 || v[0] > 63 || !hz10(v[1]) || v[2] < 0 || v[2] > 2 || v[3] < 0 || v[3] > 1) {
      say("F 1..63, FR 1-5 or 9-12, KIND 0 NOHOLD 1 HOLD 2 BALANCED, POL 0|1\n");
      return;
    }
    static const speed::LutKind KINDS[3] = {speed::LutKind::NOHOLD, speed::LutKind::HOLD,
                                            speed::LutKind::BALANCED};
    lutSet = {KINDS[v[2]], v[0], v[1], v[3] != 0};
    ui::benchSetLut(lutSet.kind, lutSet.frames, lutSet.fr, lutSet.pol);
    rsvp::benchResetStats();
    const int h = hz10(v[1]);
    say("ok lut %s F %d  FR %d (%d.%d Hz)  pol %d  drive ~%d ms\n",
        speed::lutKindName(lutSet.kind), v[0], v[1], h / 10, h % 10, v[3], v[0] * 10000 / h);
  } else if (!strcmp(cmd, "np")) {
    // No previous: skip the 0x26 write under the custom LUT. Only valid with
    // a HOLD LUT (lut F FR 1), where every transition ignores the previous
    // frame; NOHOLD and BALANCED would leave old words on glass.
    if (!needHooks() || !parseFlag(arg, f)) return;
    ui::benchSetSkipPrevious(f);
    rsvp::benchResetStats();
    say("ok np %d (HOLD LUT only)\n", f);
  } else if (!strcmp(cmd, "spi")) {
    // Above 20 MHz is past the SSD1683 write spec (fSCL max 20 MHz): bench
    // only, look for torn or shifted rows.
    if (!needHooks() || !parseInt(arg, 0, 80, n)) return;
    ui::benchSetSpiHz((uint32_t)n * 1000000u);
    spiMhzSet = n;
    rsvp::benchResetStats();
    say("ok spi %d MHz%s\n", n, n ? "" : " (spec)");
  } else if (!strcmp(cmd, "volt")) {
    // Source levels for the custom LUT (setVolt above).
    if (!needHooks() || !parseInt(arg, 0, 17, n)) return;
    if (n != 0 && n < 14) {
      say("want 0 or 14..17\n");
      return;
    }
    setVolt(n);
    voltSet = (uint8_t)n;
    rsvp::benchResetStats();
    say("ok volt %s%d (custom LUT only)\n", n ? "+/-" : "OTP ", n);
  } else if (!strcmp(cmd, "video")) {
    // video [F]: F = HOLD LUT frames at 50 Hz for this run (default: the
    // speed read waveform in force); the console LUT state is put back after.
    if (!needHooks()) return;
    n = 0;
    if (arg && !parseInt(arg, 1, 63, n)) return;
    video(n);
  } else if (!strcmp(cmd, "play")) {
    say("%s", rsvp::benchPlay() ? "ok play\n" : "not in speed read (or at the end)\n");
  } else if (!strcmp(cmd, "pause")) {
    say("%s", rsvp::benchPause() ? "ok pause\n" : "not in speed read\n");
  } else {
    say("? %s (try help)\n", cmd);
  }
}

// Streamed 1 bit video (tools/video_stream.py). Pull based so the host never
// outruns the panel: the board prints "VRDY" and the host answers with one
// frame, 'F' (partial) or 'G' (full refresh) plus VID_W x VID_H pixels as
// monob rows (MSB first, 1 = white), centred on the canvas; 'Q' ends. Runs
// with the speed read drive (custom LUT, keep power), blocks the main loop,
// and leaves after 3 s without a frame. Bench only.
const int VID_W = 368;  // 4:3 at the bar's 272 rows, rounded up to a byte
const int VID_H = 272;
const int VID_STRIDE = VID_W / 8;
uint8_t vidRow[VID_W / 8];

uint8_t rev8(uint8_t b) {
  b = (uint8_t)((b & 0xF0) >> 4 | (b & 0x0F) << 4);
  b = (uint8_t)((b & 0xCC) >> 2 | (b & 0x33) << 2);
  return (uint8_t)((b & 0xAA) >> 1 | (b & 0x55) << 1);
}

bool readExact(uint8_t *dst, size_t n, uint32_t timeoutMs) {
  const uint32_t t0 = millis();
  size_t got = 0;
  while (got < n) {
    const int a = Serial.available();
    if (a > 0) {
      got += Serial.readBytes(dst + got, min((size_t)a, n - got));
    } else {
      if (millis() - t0 > timeoutMs) return false;
      esp_task_wdt_reset();
      delay(1);
    }
  }
  return true;
}

// PackBits (the TIFF / Apple scheme): n 0..127 = n + 1 literal bytes follow,
// n 129..255 = the next byte 257 - n times, 128 = no op. Returns false on a
// stream that would overrun either side.
bool unpackBits(const uint8_t *src, size_t srcLen, uint8_t *dst, size_t dstLen) {
  size_t i = 0, o = 0;
  while (i < srcLen && o < dstLen) {
    const uint8_t n = src[i++];
    if (n < 128) {
      const size_t c = (size_t)n + 1;
      if (i + c > srcLen || o + c > dstLen) return false;
      memcpy(dst + o, src + i, c);
      i += c;
      o += c;
    } else if (n > 128) {
      const size_t c = 257 - (size_t)n;
      if (i >= srcLen || o + c > dstLen) return false;
      memset(dst + o, src[i++], c);
      o += c;
    }
  }
  return o == dstLen;
}

uint8_t *vidPacked = nullptr;  // PSRAM, one packed frame
uint8_t *vidCanvas = nullptr;  // PSRAM, one unpacked canvas (rotation 2 only)
const size_t VID_PACKED_MAX = 32768;

// Frame kinds: 'F' / 'G' = a VID_W x VID_H raw rectangle, centred (partial /
// full refresh); 'P' / 'K' = a whole canvas (stride x VID_H) as PackBits,
// 2 byte little endian length first (partial / full). The next VRDY goes out
// as soon as a frame is in the buffer, before the refresh, so the host sends
// frame n + 1 while the panel is busy with frame n (the receive queue holds
// a whole packed frame).
void video(int lutFrames) {
  if (rsvp::benchActive()) {
    say("leave speed read first\n");
    return;
  }
  if (!vidPacked) vidPacked = (uint8_t *)ps_malloc(VID_PACKED_MAX);
  if (!vidCanvas) vidCanvas = (uint8_t *)ps_malloc(VID_PACKED_MAX);
  if (!vidPacked || !vidCanvas) {
    say("no PSRAM for video\n");
    return;
  }
  // The native USB serial (HWCDC) drops what does not fit its receive queue
  // (256 bytes by default), so it has to hold a whole frame plus the next
  // one's header. Only for the run; the console goes back to the default.
  Serial.setRxBufferSize(VID_PACKED_MAX);
  const ui::BenchDrive before = ui::benchDrive();
  if (lutFrames) ui::benchSetLut(speed::LutKind::HOLD, lutFrames, 2, false);
  ui::setSpeedDrive(true);
  uint32_t frames = 0, t0 = millis();
  bool ok = true;
  Serial.print("VRDY\n");
  while (ok) {
    uint8_t kind = 0;
    if (!readExact(&kind, 1, 3000)) {
      say("video: no frame for 3 s\n");
      break;
    }
    if (kind == 'Q') break;
    const bool packed = kind == 'P' || kind == 'K';
    if (!packed && kind != 'F' && kind != 'G') {
      say("video: stray byte 0x%02x\n", kind);
      continue;
    }
    size_t plen = 0;
    if (packed) {
      uint8_t l2[2];
      if (!readExact(l2, 2, 1000)) {
        say("video: no length\n");
        break;
      }
      plen = (size_t)l2[0] | (size_t)l2[1] << 8;
      if (plen > VID_PACKED_MAX || !readExact(vidPacked, plen, 2000)) {
        say("video: short packed frame (%u bytes, %d available)\n", (unsigned)plen, Serial.available());
        break;
      }
    }
    ui::frameBeginRaw(kind == 'G' || kind == 'K');
    uint8_t *buf = nullptr;
    size_t len = 0;
    if (!ui::frameBytes(buf, len)) {
      say("no frame buffer\n");
      break;
    }
    const bool flip = ui::frameFlipped();
    const int stride = (int)(len / VID_H);  // whole canvas width, 99 on the bar
    if (packed) {
      // Unpacked straight into the frame at rotation 0; via the canvas copy
      // at rotation 2, which reverses rows, bytes and bits.
      if (!unpackBits(vidPacked, plen, flip ? vidCanvas : buf, len)) {
        say("bad packed frame\n");
        break;
      }
      if (flip) {
        for (int y = 0; y < VID_H; y++) {
          const uint8_t *src = vidCanvas + (size_t)y * stride;
          uint8_t *dst = buf + (size_t)(VID_H - 1 - y) * stride;
          for (int i = 0; i < stride; i++) dst[stride - 1 - i] = rev8(src[i]);
        }
      }
    } else {
      const int x0 = (stride - VID_STRIDE) / 2;
      for (int y = 0; y < VID_H && ok; y++) {
        ok = readExact(vidRow, VID_STRIDE, 1000);
        if (!ok) break;
        if (!flip) {
          memcpy(buf + (size_t)y * stride + x0, vidRow, VID_STRIDE);
        } else {
          uint8_t *dst = buf + (size_t)(VID_H - 1 - y) * stride;
          for (int i = 0; i < VID_STRIDE; i++) dst[stride - 1 - (x0 + i)] = rev8(vidRow[i]);
        }
      }
      if (!ok) break;
    }
    Serial.print("VRDY\n");
    while (ui::frameNext()) {
    }
    frames++;
  }
  ui::frameEnd();
  ui::setSpeedDrive(false);
  if (lutFrames) restoreLut(before);
  Serial.setRxBufferSize(256);
  const uint32_t ms = millis() - t0;
  say("VEND %lu frames  %lu ms  %lu.%lu fps\n", (unsigned long)frames, (unsigned long)ms,
      (unsigned long)(ms ? frames * 1000 / ms : 0), (unsigned long)(ms ? frames * 10000 / ms % 10 : 0));
}

}  // namespace

namespace bench {

void begin() {
  lineLen = 0;
  overflow = false;
  // No drive defaults here: the panel's speed profile (src/panel.cpp,
  // src/speed_profile.h) sets them, and console commands are overrides on
  // top until `clear` or a reboot.
}

void poll() {
  // Bounded per pass so a flood of input cannot hold the loop.
  for (int budget = 128; budget > 0 && Serial.available() > 0; budget--) {
    const int c = Serial.read();
    if (c < 0) break;
    if (c == '\n' || c == '\r') {
      if (!overflow && lineLen) {
        line[lineLen] = 0;
        run(line);
      }
      lineLen = 0;
      overflow = false;
    } else if (lineLen < CMD_LINE_MAX) {
      line[lineLen++] = (char)c;
    } else {
      overflow = true;
    }
  }
}

}  // namespace bench
