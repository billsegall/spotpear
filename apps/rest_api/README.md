# rest_api

The board joins WiFi over DHCP and exposes a small REST API to drive the display
and audio. It composes the GC9A01 display (`display_image`) and ES8311 audio
(`play_audio`) behind an HTTP server, and adds mic recording and on-panel text.

## Setup

1. **Credentials.** Edit `apps/rest_api/.env` (created for you, gitignored):

   ```
   WIFI_SSID=4PrivetDrive
   WIFI_PASSWORD=your-password-here
   API_TOKEN=                 # blank = open API; set a value to require X-Api-Key
   ```

   `.env` and the generated `main/wifi_credentials.h` are **never committed** —
   the repo ships only `tools/gen_credentials.py`, which bakes them into the
   firmware at build time.

2. **Build and flash:**

   ```sh
   . ~/esp/esp-idf/export.sh
   idf.py -p /dev/ttyACM0 flash monitor
   ```

   The board shows its DHCP IP on the panel (and prints it on serial) once
   connected. Use that IP below.

## Endpoints

| Method + path | Body | Effect |
| --- | --- | --- |
| `GET /health` | — | JSON: status, IP, free heap |
| `POST /display/image` | JPEG **or** raw RGB565 | show a 240×240 image |
| `POST /display/text` | JSON `{"text":"...","fg":[r,g,b],"bg":[r,g,b]}` | show text |
| `POST /play` | 16-bit PCM WAV | play through the speaker |
| `GET /record?seconds=n` | — | record the mic (1–30 s), return a WAV |

If `API_TOKEN` is set, every request needs `-H "X-Api-Key: <token>"`.

### Examples

```sh
IP=192.168.1.42

curl "http://$IP/health"

# Image — a 240x240 JPEG (decoded on-device) ...
curl --data-binary @pic.jpg -H 'Content-Type: image/jpeg' "http://$IP/display/image"
# ... or pre-converted raw RGB565 (reuses tools/png_to_rgb565.py):
./tools/png_to_rgb565.py pic.jpg pic.rgb565
curl --data-binary @pic.rgb565 -H 'Content-Type: application/octet-stream' "http://$IP/display/image"

# Text
curl -H 'Content-Type: application/json' \
  -d '{"text":"Hello, 4 Privet Drive","fg":[0,255,0],"bg":[0,0,40]}' \
  "http://$IP/display/text"

# Play a WAV
curl --data-binary @clip.wav "http://$IP/play"

# Record 5 s of mic audio
curl "http://$IP/record?seconds=5" -o out.wav
```

## How it works / constraints

- **Memory is the binding constraint.** With WiFi up the C3 has ~50 KB free heap,
  and its largest contiguous block is ~112 KB — under the 115 KB a full 240×240
  RGB565 frame needs. So the framebuffer is **two 120-row halves in ordinary RAM**
  (not the scarce DMA region, which WiFi and the codec need), pushed to the panel
  one small band at a time through a DMA bounce buffer, waiting for each band's
  transfer before reusing it. JPEG decodes **block by block** straight into that
  bounce, needing no full-frame output buffer. JPEG input is capped at 48 KB.
- **Audio** uses the ES8311 in duplex (`WORK_MODE_BOTH`): one output device for
  `/play`, one input device for `/record`, sharing the codec + I2S. The
  esp_codec_dev I2S path requires an even channel count, so both run 2-channel and
  mono is duplicated (play) / the left channel is taken (record). Uploads and
  recordings stream through a SPIFFS file, never a whole clip in RAM.
- **Credentials** are baked into the firmware image from `.env` (acceptable for a
  home board; captive-portal provisioning is the heavier alternative). Every
  handler serializes on a mutex — the display and codec are shared hardware.

## Verifying

`GET /health` and each command are curl-testable from the host, and a returned
recording can be RMS-checked to confirm it is non-silent. What curl cannot confirm
— that the image actually renders and the sound actually leaves the speaker —
needs your eyes and ears.
