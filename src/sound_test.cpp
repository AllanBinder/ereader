#include "sound_test.h"

#include <driver/gpio.h>
#include <driver/i2s.h>
#include <esp_task_wdt.h>
#include <math.h>

#include "pins.h"

// Same sweep, settings and legacy driver/i2s.h API as the bench test in
// src/dac_tone_test.cpp ([env:dac_tone]); keep the two in step.

namespace {

const i2s_port_t I2S_PORT = I2S_NUM_0;
const int SAMPLE_RATE = 44100;
const float AMPLITUDE = 8192.0f;  // 32767 / 4, about -12 dBFS
const int FRAMES_PER_CHUNK = 256;

struct Segment {
  const char *label;
  float freq;  // 0 = silence
  bool left;
  bool right;
  uint32_t ms;
};

const Segment SEGMENTS[] = {
    {"tone 440 both", 440.0f, true, true, 1000},
    {"silence", 0.0f, false, false, 500},
    {"tone 880 left", 880.0f, true, false, 1000},
    {"tone 880 right", 880.0f, false, true, 1000},
};
const int NUM_SEGMENTS = sizeof(SEGMENTS) / sizeof(SEGMENTS[0]);

}  // namespace

namespace sound_test {

bool fitted() {
  return PIN_I2S_BCK >= 0 && PIN_I2S_LCK >= 0 && PIN_I2S_DIN >= 0;
}

bool run() {
  if (!fitted()) return false;

  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate = SAMPLE_RATE;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = 0;
  cfg.dma_buf_count = 8;
  cfg.dma_buf_len = FRAMES_PER_CHUNK;
  cfg.use_apll = false;
  cfg.tx_desc_auto_clear = true;  // underrun plays silence, not a stale buffer
  cfg.fixed_mclk = 0;

  i2s_pin_config_t pins = {};
  pins.mck_io_num = I2S_PIN_NO_CHANGE;  // module SCK tied to GND, no MCLK
  pins.bck_io_num = PIN_I2S_BCK;
  pins.ws_io_num = PIN_I2S_LCK;
  pins.data_out_num = PIN_I2S_DIN;
  pins.data_in_num = I2S_PIN_NO_CHANGE;

  esp_err_t err = i2s_driver_install(I2S_PORT, &cfg, 0, NULL);
  if (err != ESP_OK) {
    Serial.printf("sound test: i2s install %s\n", esp_err_to_name(err));
    return false;
  }
  err = i2s_set_pin(I2S_PORT, &pins);
  if (err != ESP_OK) {
    Serial.printf("sound test: i2s pins %s\n", esp_err_to_name(err));
    i2s_driver_uninstall(I2S_PORT);
    return false;
  }
  Serial.printf("sound test: BCK %d, LCK %d, DIN %d\n", PIN_I2S_BCK, PIN_I2S_LCK,
                PIN_I2S_DIN);

  static int16_t chunk[FRAMES_PER_CHUNK * 2];  // interleaved L, R
  for (int s = 0; s < NUM_SEGMENTS; s++) {
    const Segment &seg = SEGMENTS[s];
    Serial.printf("sound test: %s\n", seg.label);
    float phase = 0.0f;  // each tone starts at zero crossing, no click
    const float step = 2.0f * (float)M_PI * seg.freq / SAMPLE_RATE;
    uint32_t framesLeft = (uint32_t)SAMPLE_RATE * seg.ms / 1000;
    while (framesLeft > 0) {
      int n = framesLeft < (uint32_t)FRAMES_PER_CHUNK ? (int)framesLeft : FRAMES_PER_CHUNK;
      for (int i = 0; i < n; i++) {
        int16_t v = 0;
        if (seg.freq > 0.0f) {
          v = (int16_t)(AMPLITUDE * sinf(phase));
          phase += step;
          if (phase >= 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
        }
        chunk[2 * i] = seg.left ? v : 0;
        chunk[2 * i + 1] = seg.right ? v : 0;
      }
      size_t written = 0;
      i2s_write(I2S_PORT, chunk, n * 2 * sizeof(int16_t), &written, portMAX_DELAY);
      framesLeft -= n;
    }
    esp_task_wdt_reset();
  }

  // Let the DMA buffers drain (8 x 256 frames, about 46 ms) and go silent
  // before the driver goes, then hand the pins back as plain GPIO.
  delay(60);
  i2s_zero_dma_buffer(I2S_PORT);
  i2s_driver_uninstall(I2S_PORT);
  gpio_reset_pin((gpio_num_t)PIN_I2S_BCK);
  gpio_reset_pin((gpio_num_t)PIN_I2S_LCK);
  gpio_reset_pin((gpio_num_t)PIN_I2S_DIN);
  return true;
}

}  // namespace sound_test
