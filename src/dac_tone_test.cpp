// Bench tone test for a PCM5102A I2S DAC module on the wide reader. Built only
// by [env:dac_tone]; every other env compiles nothing from this file.
// Sequence, looping: 440 Hz both channels 1 s, silence 0.5 s, 880 Hz LEFT
// only 1 s, 880 Hz RIGHT only 1 s. 44.1 kHz, 16 bit stereo, about -12 dBFS.
// Uses the legacy driver/i2s.h API (this platform resolves Arduino core 2.x).

#if defined(DAC_TONE_TEST) && DAC_TONE_TEST

#include <Arduino.h>
#include <driver/i2s.h>
#include <math.h>

#include "pins.h"

static const i2s_port_t I2S_PORT = I2S_NUM_0;
static const int SAMPLE_RATE = 44100;
static const float AMPLITUDE = 8192.0f;  // 32767 / 4, about -12 dBFS
static const int FRAMES_PER_CHUNK = 256;

struct Segment {
  const char *label;
  float freq;  // 0 = silence
  bool left;
  bool right;
  uint32_t ms;
};

static const Segment SEGMENTS[] = {
    {"tone 440 both", 440.0f, true, true, 1000},
    {"silence", 0.0f, false, false, 500},
    {"tone 880 left", 880.0f, true, false, 1000},
    {"tone 880 right", 880.0f, false, true, 1000},
};
static const int NUM_SEGMENTS = sizeof(SEGMENTS) / sizeof(SEGMENTS[0]);

static int16_t chunk[FRAMES_PER_CHUNK * 2];  // interleaved L, R
static float phase = 0.0f;

void setup() {
  Serial.begin(115200);
  delay(500);

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
  if (err == ESP_OK) err = i2s_set_pin(I2S_PORT, &pins);
  Serial.printf("dac_tone: i2s init %s (BCK %d, LCK %d, DIN %d)\n",
                err == ESP_OK ? "ok" : esp_err_to_name(err), PIN_I2S_BCK,
                PIN_I2S_LCK, PIN_I2S_DIN);
}

void loop() {
  for (int s = 0; s < NUM_SEGMENTS; s++) {
    const Segment &seg = SEGMENTS[s];
    Serial.println(seg.label);
    phase = 0.0f;  // each tone starts at zero crossing, no click
    const float step = 2.0f * (float)M_PI * seg.freq / SAMPLE_RATE;
    uint32_t frames_left = (uint32_t)SAMPLE_RATE * seg.ms / 1000;
    while (frames_left > 0) {
      int n = frames_left < FRAMES_PER_CHUNK ? frames_left : FRAMES_PER_CHUNK;
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
      i2s_write(I2S_PORT, chunk, n * 2 * sizeof(int16_t), &written,
                portMAX_DELAY);
      frames_left -= n;
    }
  }
}

#endif  // DAC_TONE_TEST
