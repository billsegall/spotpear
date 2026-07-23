#pragma once

#include "esp_err.h"

#define AUDIO_RECORD_RATE 24000  // ES8311 mic capture rate

// Init the ES8311 codec for duplex (play + record). Call once.
esp_err_t audio_init(void);

// Play a 16-bit PCM WAV file (mono or stereo, any sample rate) to the speaker.
esp_err_t audio_play_wav_file(const char *path);

// Record `seconds` of mono audio from the mic into a 16-bit PCM WAV file.
esp_err_t audio_record_wav_file(const char *path, int seconds);
