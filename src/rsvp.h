#pragma once

#include <Arduino.h>

#include "books.h"
#include "input.h"
#include "speed_profile.h"

// Speed read: RSVP (rapid serial visual presentation) over the open book.
//
// A few words at a time are flashed in one fixed spot on the landscape panel,
// each chunk drawn in the largest Gelasio size that fits, on the partial LUT
// with only the changed pixels driven. Ported from the rsvp_eink proof of
// concept, whose timing and waveform settings were tuned on panel 0 (see the
// RSVP_* constants in config.h). Panels that turn speed read on carry their
// own SPI clock, top speeds and ghost clearing cadence in panel::Spec; the
// layout follows the canvas height (a tall frame like panel 0's 920x680, or
// the short 792x272 bar of the wide reader). Waveform cut and white push run
// only on a driver with those hooks (ui::speedHooksActive). A panel whose row
// carries speed profiles (the 5.79 inch GDEY0579T93) takes its top speeds and
// ghost clearing cadence from the profile of the speed mode instead (Normal or
// Turbo, see src/speed_profile.h and mode() below).
//
// While playing, the HUD shows the achieved speed next to the rate ("REAL
// 412") once the panel falls below 90% of the set speed.
//
// The mode reads pages through the Book it is handed and keeps the book's
// reading position in step: whenever the chunk on glass starts on a new page,
// that page is saved exactly as a page turn would save it, so the menu and the
// normal reading view pick up where speed read left off.
//
// main.cpp owns the state machine. It calls enter(), then tick() from every
// loop() pass and handle() for every button, and leave() once handle() asks to
// exit, after which it draws the reading page with a full refresh.

namespace rsvp {

// Loads the stored speed and speed mode from NVS and hands the mode to
// ui::setSpeedMode. Call once at boot, after the panel is up.
void begin();

// Words per minute, as shown on the menu tile.
int wpm();

// Words per flash, 1-3.
int words();

// Picker settings tiles: step words per flash 1 -> 2 -> 3 -> 1 (clamping the
// speed to that mode's top speed on the active panel), and step the speed up
// by RSVP_WPM_STEP, wrapping to RSVP_WPM_MIN past that top. Both are saved to
// NVS.
void cycleWords();
void cycleWpm();

// Speed mode (Normal, the default, or Turbo), stored in NVS. setMode passes
// it to ui::setSpeedMode (re-applied at once while the drive is on), clamps
// the speed to the new mode's top speed and saves both. cycleMode toggles
// Normal <-> Turbo for the picker's Mode tile. On a panel without speed
// profiles (ui::activeSpeedProfile() nullptr) setMode and cycleMode do
// nothing; mode() reports the stored mode.
speed::Mode mode();
void setMode(speed::Mode m);
void cycleMode();

// In-page word to restart at when speed read is reopened on `page` of the book
// and variant it was last left on, so a pause and a look at the page does not
// lose the reader's place inside it. RAM only; 0 for anything else.
uint16_t resumeWord(const String &slug, uint8_t variant, uint32_t page);

// Starts on `book` (open, active variant already chosen) at word `word` of
// `page`. An image page or a page with no words moves on to the next page that
// has some. Switches the panel to landscape and the fast drive and shows the
// first chunk with a full refresh.
void enter(Book &book, uint32_t page, uint16_t word);

// Flashes the next chunk when it is due. Returns true when it drew a frame,
// so the caller can flush the input that piled up while the panel was busy.
bool tick();

// One button. Returns true when the reader asked to leave (CENTER while
// paused or at the end); the caller then calls leave(). The wide reader build
// (PINS_WIDE) has its own mapping, UP and DOWN stepping the speed and RIGHT
// the words per flash; see rsvp.cpp.
bool handle(Button b);

// Puts the panel back to portrait and the normal drive, and remembers the word
// for resumeWord(). The caller follows with a full refresh of the page.
void leave();

// Page holding the first word of the chunk on glass: the reading position.
uint32_t page();

// ------------------------------------------------------------ bench console
//
// Used only by src/bench.cpp, the serial console for tuning the fast panel 2
// driver. None of these save to NVS. Without a console command the reader
// behaves exactly as without them.

// True between enter() and leave().
bool benchActive();
bool benchPlaying();
// Speed, RSVP_WPM_MIN..1000 (BENCH_WPM_MAX in rsvp.cpp), past the top speed
// of the panel and speed mode (enter() and setMode keep it).
void benchSetWpm(int wpm);
// Words per flash 1-3, the speed left as it is (no clamp).
void benchSetWords(int n);
// Ghost clearing full refresh at the next due time.
void benchForceFull();
// White push (drive all) on the next partial flash only.
void benchPushOnce();
// Turbo ghost clearing: two drive all partials (black, then blank) in place
// of the full refresh. On by default.
void benchSetSoftClear(bool on);
// One serial line per playback flash (timing, cut, hooks).
void benchSetLog(bool on);
// Incremental compose: when nothing but the words changed, redraw only the
// words band over the kept frame buffer (bit for bit the full compose; see
// render() in rsvp.cpp). On by default. Off = the whole screen composed every
// flash.
void benchSetIncremental(bool on);
// Band window: when nothing but the words changed, send only the band's rows
// in a partial window. Off by default; ignored on panel 0, whose driver
// always ships a whole frame (ui::windowFrames).
void benchSetWindow(bool on);
// Glyph blit: the chunk's words drawn straight into the frame buffer bytes
// (ui::blitText), bit for bit the GFX print() frame without one virtual
// drawPixel per pixel. On by default; it falls back to print() by itself
// where the buffer is not addressable or the rotation is portrait.
void benchSetBlit(bool on);
// Identity check of the blit: draws the chunk on glass both ways into the
// live buffer (at the current rotation and the flipped one, over a white and
// a striped background), counts the bytes that differ, then puts the buffer
// back as it was. Nothing is sent to the panel.
struct BlitCheck {
  bool ran;
  const char *why;   // why it did not run
  size_t bytes;      // buffer bytes compared per pass
  int mismatch;      // differing bytes, current rotation (both backgrounds)
  int mismatchFlip;  // the same at the flipped rotation
  int fellBack;      // passes where the blit declined and print() drew
  uint32_t gfxUs;    // last pass: print() path, placement included
  uint32_t blitUs;   // last pass: blit path, placement included
};
BlitCheck benchBlitCheck();
// Play / pause as the keys do; false when not in speed read (or at the end).
bool benchPlay();
bool benchPause();
// Empties the rolling stats; bench.cpp calls it after every setting change.
void benchResetStats();

// Rolling stats over the last 32 playback flashes (frameBeginRaw to
// frameEnd), plus the current settings.
struct BenchStats {
  int wpm;
  int words;
  uint32_t intervalMs;  // nominal time per flash at this speed
  int cutMs;            // cut a flash of that length gets (0 = none)
  uint32_t flashes;     // playback flashes since enter()
  int partialsSinceFull;
  bool fullPending;
  bool pushPending;
  bool log;
  bool incremental;     // benchSetIncremental
  bool window;          // benchSetWindow
  bool windowCapable;   // the driver honours a window (not panel 0)
  bool blit;            // benchSetBlit
  bool blitCapable;     // ui::blitAvailable() right now
  int count;            // flashes in the window below, up to 32
  uint32_t avgMs, minMs, maxMs;
  uint32_t avgComposeMs;  // of avgMs, inside compose
  uint32_t avgOutMs;      // of avgMs, the rest: transfer + refresh
  uint32_t avgGapMs;    // idle time between a frame's end and the next start
  int late;             // frame longer than its hold, or start 20+ ms late
  int bandFlashes;      // composed band only over the kept buffer
  int windowFlashes;    // sent as a band window
  int blitFlashes;      // words drawn through the glyph blit
  // Words shown per minute of real time between flash starts (lateness and
  // ghost clearing fulls included), and per minute of the holds the flashes
  // were planned with (sentence and clause dwell included). 0 = too few.
  int achievedWpm;
  int plannedWpm;
};
BenchStats benchStats();

}  // namespace rsvp
