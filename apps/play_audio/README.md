# play_audio

Plays a raw PCM audio file through the board's ES8311 codec + NS4150B amp,
looping forever. No decoding on-device: the file is pre-converted on the host to
the codec's native format and streamed straight to the DAC.

## Get the audio and flash

```sh
./tools/fetch_audio.sh          # downloads + converts -> main/audio_fs/audio.pcm
. ~/esp/esp-idf/export.sh
idf.py -p /dev/ttyACM0 flash     # writes app AND audio in one pass
```

The first track is The Trashmen — "Surfin' Bird", from
`https://audio.com/ed-barriager/audio/the-trashmen-surfin-bird`.

### Licensing — why the audio is not in the repo

"Surfin' Bird" is a copyrighted commercial recording. **No audio is committed**:
`*.mp3`, `*.pcm`, `*.wav` and `audio_fs/` are gitignored (like the vendor
schematic). The repo ships `tools/fetch_audio.sh`, which reproduces the asset
locally. Anyone building this fetches their own copy; nothing copyrighted is
pushed.

## Use your own audio

Replace `main/audio_fs/audio.pcm` with **mono, 24 kHz, signed 16-bit
little-endian raw PCM** and reflash:

```sh
ffmpeg -i yourfile.mp3 -ac 1 -ar 24000 -c:a pcm_s16le -f s16le main/audio_fs/audio.pcm
idf.py -p /dev/ttyACM0 flash
```

Keep it under ~13 MB (the SPIFFS `assets` partition) — that's ~4.5 minutes at
this format. Only files inside `main/audio_fs/` are flashed; keep intermediates
elsewhere.

## How it works

- **No on-device decode.** Converting to raw PCM on the host is
  correct-by-construction and verifiable there (`fetch_audio.sh` checks the
  result is the right length and non-silent). Decoding MP3 on the single-core C3
  with no PSRAM is avoided; the cost is storage, which the 16 MB flash absorbs.
- **Storage.** `partitions.csv` defines a single app plus a ~13.9 MB SPIFFS
  `assets` partition. `spiffs_create_partition_image(... FLASH_IN_PROJECT)` in
  `main/CMakeLists.txt` builds the audio image and `idf.py flash` writes it in the
  same pass — no manual `esptool` offset to remember.
- **Codec.** Pins and config come from the vendor stock firmware
  ([`docs/vendor/config.h`](../../docs/vendor/config.h),
  [`toy_ai_core_c3_1.28.cc`](../../docs/vendor/toy_ai_core_c3_1.28.cc)):
  I2C SDA 3 / SCL 4, ES8311 @ 0x18; I2S MCLK 10 / BCLK 8 / WS 6 / DOUT 5; the C3
  is I2S master, the codec a slave. Driven through the registry
  `espressif/esp_codec_dev` component (declared in `main/idf_component.yml`), which
  also pulls `es8311`.
- **Mono vs stereo.** The file is mono, but `esp_codec_dev`'s I2S data path only
  accepts an **even** channel count, so the app opens the codec as 2-channel and
  duplicates each mono sample into L and R before writing. Get this wrong and you
  get wrong-speed or one-channel playback that the serial log won't show.
- **PA pin.** The NS4150B amp is muted unless GPIO 11 is high. The `es8311`
  driver drives it via `es8311_cfg.pa_pin` when the codec opens — no manual GPIO.
- **No restart**, unlike [`hello_world`](../hello_world) — a reboot would gap the
  loop. `sdkconfig.defaults` sets 16 MB flash + the custom partition table.

## Verifying

`fetch_audio.sh` proves the *asset* on the host (length ≈ 142 s, non-silent RMS).
The serial log proves the firmware path: SPIFFS mounted, codec opened, playback
loop entered, no reset loop. Neither proves sound actually leaves the speaker —
that needs your ears. If it's silent, first suspects are the PA pin and the
mono/stereo channel config above.
