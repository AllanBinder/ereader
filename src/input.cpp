#include "input.h"

#include <Arduino.h>
#include "config.h"
#include "pins.h"

#include <driver/gpio.h>
#include <esp_timer.h>


namespace {

// Physical inputs: the five d-pad lines first (index = Button), then the two
// shoulder keys. A key with pin -1 is not fitted on this build and is skipped.
const int NUM_INPUTS = BTN_COUNT + 2;
const int kPins[NUM_INPUTS] = {PIN_BTN_UP, PIN_BTN_DOWN, PIN_BTN_LEFT,
                               PIN_BTN_RIGHT, PIN_BTN_CENTER,
                               PIN_BTN_K1, PIN_BTN_K2};
// What each shoulder key does, K1 (left key) and K2 (right key) as seen from
// the front. Reversed on purpose (founder, 2026-11-06): the right key goes
// LEFT. Change a role here, nothing else needs to know.
const Button KEY_ACTS_AS[2] = {BTN_RIGHT, BTN_LEFT};
const char kNames[NUM_INPUTS + 1] = "UDLRC12";

bool fitted(int i) {
  if (i == BTN_LEFT && PAD_LEFT_OFF) return false;
  return kPins[i] >= 0;
}

bool stable[NUM_INPUTS];      // debounced level, true = pressed
bool lastRaw[NUM_INPUTS];     // last sampled level, true = pressed
uint32_t lastChange[NUM_INPUTS];
bool pendingEvent[NUM_INPUTS];
int idleLevel[NUM_INPUTS];    // sampled at boot; pressed = the other level
uint32_t lastAcceptedPress = 0;  // millis of the last press turned into an event
bool haveAccepted = false;
bool padTurned = false;  // see input::setTurned()
int lastSrc = -1;        // see input::lastSource()

// Physical index to the button it means right now: shoulder keys collapse
// onto their d-pad role first, pad lines take the LEFT/RIGHT swap, then the
// turn swap applies. PAD_FLIP is about the pad module, so it turns the pad
// lines only; the keys turn with the picture alone. For the five pad lines
// each swap is its own inverse and they commute, so this also maps a button
// back to its pin.
int logicalOf(int i) {
  bool turn;
  if (i >= BTN_COUNT) {
    i = KEY_ACTS_AS[i - BTN_COUNT];
    turn = padTurned;
  } else {
    if (PAD_SWAP_LR && i == BTN_LEFT) i = BTN_RIGHT;  // see include/pins.h
    else if (PAD_SWAP_LR && i == BTN_RIGHT) i = BTN_LEFT;
    turn = padTurned != PAD_FLIP;  // PAD_FLIP: see include/pins.h
  }
  if (!turn) return i;
  switch (i) {
    case BTN_UP: return BTN_DOWN;
    case BTN_DOWN: return BTN_UP;
    case BTN_LEFT: return BTN_RIGHT;
    case BTN_RIGHT: return BTN_LEFT;
    default: return i;
  }
}

// Interrupt side of the latch. A press edge stamps fallUs; the release edge
// latches the button when the pin stayed active long enough to be a finger
// and not a glitch coupled in by the panel refresh.
volatile int64_t fallUs[NUM_INPUTS];
volatile bool latched[NUM_INPUTS];

void IRAM_ATTR padIsr(void *arg) {
  int i = (int)(intptr_t)arg;
  bool active = gpio_get_level((gpio_num_t)kPins[i]) != idleLevel[i];
  int64_t now = esp_timer_get_time();
  if (active) {
    fallUs[i] = now;
  } else if (fallUs[i] && now - fallUs[i] >= (int64_t)BUTTON_LATCH_MS * 1000) {
    latched[i] = true;
    fallUs[i] = 0;
  } else {
    fallUs[i] = 0;
  }
}

}  // namespace

namespace input {

void begin() {
  uint32_t now = millis();
  for (int i = 0; i < NUM_INPUTS; i++) {
    if (fitted(i)) pinMode(kPins[i], INPUT_PULLUP);
    stable[i] = false;
    lastRaw[i] = false;
    lastChange[i] = now;
    pendingEvent[i] = false;
  }
  // D-pad modules differ: some short to GND when pressed (idle HIGH), some
  // short to VCC (idle LOW, onboard pulldown outweighs our weak pullup).
  // Sample the idle level once at boot and call any deviation a press.
  // Consequence: do not hold a button through reset.
  delay(5);
  for (int i = 0; i < NUM_INPUTS; i++) {
    idleLevel[i] = fitted(i) ? digitalRead(kPins[i]) : HIGH;
  }
  Serial.printf("buttons idle U%d D%d L%d R%d C%d (gpio %d %d %d %d %d)\n", idleLevel[0],
       idleLevel[1], idleLevel[2], idleLevel[3], idleLevel[4], kPins[0],
       kPins[1], kPins[2], kPins[3], kPins[4]);
  for (int i = 0; i < NUM_INPUTS; i++) {
    fallUs[i] = 0;
    latched[i] = false;
    if (fitted(i)) attachInterruptArg(kPins[i], padIsr, (void *)(intptr_t)i, CHANGE);
  }
  if (fitted(BTN_COUNT))
    Serial.printf("shoulder keys K1 K2 gpio %d %d\n", kPins[5], kPins[6]);
}

bool takeLatched(Button &out) {
  int64_t now = esp_timer_get_time();
  for (int i = 0; i < NUM_INPUTS; i++) {
    // A pad still held when the loop comes back counts once it has been
    // down long enough, so a press that outlasts the draw is not lost either.
    if (!latched[i] && fallUs[i] &&
        now - fallUs[i] >= (int64_t)BUTTON_LATCH_MS * 1000) {
      latched[i] = true;
      fallUs[i] = 0;
    }
    if (latched[i]) {
      latched[i] = false;
      out = (Button)logicalOf(i);
      lastSrc = i;
      Serial.printf("btn %d latched press\n", i);
      return true;
    }
  }
  return false;
}

int lastSource() { return lastSrc; }

void clearLatched() {
  for (int i = 0; i < NUM_INPUTS; i++) {
    latched[i] = false;
    fallUs[i] = 0;
  }
}

void flush() {
  uint32_t now = millis();
  int dropped = 0;
  char droppedNames[NUM_INPUTS * 2 + 1] = {0};
  for (int i = 0; i < NUM_INPUTS; i++) {
    if (!fitted(i)) continue;
    if (pendingEvent[i]) {
      droppedNames[dropped * 2] = kNames[i];
      droppedNames[dropped * 2 + 1] = ' ';
      dropped++;
    }
    bool raw = (digitalRead(kPins[i]) != idleLevel[i]);
    stable[i] = raw;
    lastRaw[i] = raw;
    lastChange[i] = now;
    pendingEvent[i] = false;
  }
  if (dropped) Serial.printf("input flush: dropped %s\n", droppedNames);
}

uint8_t rawMask() {
  uint8_t m = 0;
  for (int i = 0; i < NUM_INPUTS; i++) {
    if (stable[i]) m |= (uint8_t)(1 << logicalOf(i));
  }
  return m;
}

uint8_t liveMask() {
  uint8_t m = 0;
  for (int i = 0; i < NUM_INPUTS; i++) {
    if (!fitted(i)) continue;
    if (digitalRead(kPins[i]) != idleLevel[i]) m |= (uint8_t)(1 << logicalOf(i));
  }
  return m;
}

bool poll(Button &out) {
  uint32_t now = millis();
  for (int i = 0; i < NUM_INPUTS; i++) {
    if (!fitted(i)) continue;
    bool raw = (digitalRead(kPins[i]) != idleLevel[i]);
    if (raw != lastRaw[i]) {
      lastRaw[i] = raw;
      lastChange[i] = now;
    } else if (raw != stable[i] && (now - lastChange[i]) >= BUTTON_DEBOUNCE_MS) {
      stable[i] = raw;
      // Whole-pad snapshot on every edge: a press that pulls two lines is
      // indistinguishable from two presses once poll() has picked a winner,
      // so log what all five pins read at the moment of the edge.
      char snap[NUM_INPUTS + 1];
      for (int k = 0; k < NUM_INPUTS; k++) {
        snap[k] = (fitted(k) && digitalRead(kPins[k]) != idleLevel[k]) ? '1' : '0';
      }
      snap[NUM_INPUTS] = 0;
      bool accepted = false;
      if (raw) {
        // A second line going active right behind the first is the same press
        // arriving twice over the shorted pair, so it is swallowed here rather
        // than queued as its own event.
        bool coincident =
            haveAccepted && (now - lastAcceptedPress) < BUTTON_COINCIDENCE_MS;
        if (!coincident) {
          pendingEvent[i] = true;
          lastAcceptedPress = now;
          haveAccepted = true;
          accepted = true;
          // The poller saw this press, so the interrupt latch must not hand
          // it out a second time when the pad is released.
          fallUs[i] = 0;
          latched[i] = false;
        }
      }
      Serial.printf("btn %c gpio %d %s  pad UDLRC12=%s%s\n", kNames[i], kPins[i],
                    raw ? "press" : "release", snap,
                    (raw && !accepted) ? "  [coincident, ignored]" : "");
    }
  }
  for (int i = 0; i < NUM_INPUTS; i++) {
    if (pendingEvent[i]) {
      pendingEvent[i] = false;
      out = (Button)logicalOf(i);
      lastSrc = i;
      return true;
    }
  }
  return false;
}

void setTurned(bool turned) { padTurned = turned; }

bool turned() { return padTurned; }

int gpioFor(Button b) {
  if ((int)b < 0 || (int)b >= BTN_COUNT) return -1;
  return kPins[logicalOf((int)b)];
}

}  // namespace input
