// Bench button test for the wide reader: prints every press and release of
// the five d-pad lines and the two shoulder keys over USB serial, plus a
// full state line every 5 s. Built only by [env:btn_test]; every other env
// compiles nothing from this file. Pins come from pins.h (PINS_WIDE), all
// active LOW with internal pull-ups. No display, no audio.

#if defined(BTN_TEST) && BTN_TEST

#include <Arduino.h>

#include "pins.h"

struct Btn {
  const char *name;
  int pin;
  bool down;
  uint32_t changedMs;
};

static Btn BTNS[] = {
    {"UP", PIN_BTN_UP, false, 0},       {"DOWN", PIN_BTN_DOWN, false, 0},
    {"LEFT", PIN_BTN_LEFT, false, 0},   {"RIGHT", PIN_BTN_RIGHT, false, 0},
    {"CENTER", PIN_BTN_CENTER, false, 0}, {"K1", PIN_BTN_K1, false, 0},
    {"K2", PIN_BTN_K2, false, 0},
};
static const int NUM_BTNS = sizeof(BTNS) / sizeof(BTNS[0]);
static const uint32_t DEBOUNCE_MS = 15;
static uint32_t lastStatusMs = 0;

static void printStatus() {
  Serial.print("state:");
  for (int i = 0; i < NUM_BTNS; i++) {
    Serial.printf(" %s(%d)=%s", BTNS[i].name, BTNS[i].pin,
                  BTNS[i].down ? "DOWN" : "up");
  }
  Serial.println();
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  for (int i = 0; i < NUM_BTNS; i++) {
    pinMode(BTNS[i].pin, INPUT_PULLUP);
  }
  delay(5);
  for (int i = 0; i < NUM_BTNS; i++) {
    BTNS[i].down = digitalRead(BTNS[i].pin) == LOW;
  }
  Serial.println("btn_test: press each button; DOWN at boot = stuck or shorted");
  printStatus();
}

void loop() {
  uint32_t now = millis();
  for (int i = 0; i < NUM_BTNS; i++) {
    Btn &b = BTNS[i];
    bool down = digitalRead(b.pin) == LOW;
    if (down != b.down && now - b.changedMs >= DEBOUNCE_MS) {
      b.down = down;
      b.changedMs = now;
      Serial.printf("%8lu  %-6s GPIO %2d  %s\n", (unsigned long)now, b.name,
                    b.pin, down ? "PRESS" : "release");
    }
  }
  if (now - lastStatusMs >= 5000) {
    lastStatusMs = now;
    printStatus();
  }
  delay(2);
}

#endif  // BTN_TEST
