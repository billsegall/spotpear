#!/usr/bin/env bash
# Fetch the track and convert it to the raw PCM the app plays.
#
#   ./tools/fetch_audio.sh
#
# Produces main/audio_fs/audio.pcm — mono, 24 kHz, signed 16-bit little-endian,
# the ES8311's proven native format (no on-device resampling). Both the .mp3 and
# the .pcm are gitignored: "Surfin' Bird" is a copyrighted recording, kept local,
# never committed. Re-run anytime to regenerate.
#
# audio.com gates its web pages behind a Cloudflare JS challenge, but its API is
# open. We hit the durable API endpoint for the numeric track id to mint a fresh
# presigned S3 URL each run (the signed URL itself expires in days — never cache
# it), then download and transcode.

set -euo pipefail

AUDIO_ID="1841912039727181"  # audio.com/ed-barriager/audio/the-trashmen-surfin-bird
API="https://api.audio.com/audio/${AUDIO_ID}"
UA="Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 Chrome/120.0 Safari/537.36"

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# The mp3 stays OUTSIDE main/audio_fs/ — spiffs_create_partition_image flashes
# everything in that dir (hidden files included), and only audio.pcm belongs on
# the device.
mp3="${here}/surfin.mp3"
pcm="${here}/main/audio_fs/audio.pcm"
mkdir -p "${here}/main/audio_fs"

echo "==> resolving a fresh media URL from ${API}"
url="$(curl -fsSL -A "$UA" -H 'Accept: application/json' "$API" \
  | python3 -c 'import json,sys
d=json.load(sys.stdin)
urls=[]
def walk(o):
    if isinstance(o,dict):
        for k,v in o.items():
            if k=="url" and isinstance(v,str) and ".mp3" in v: urls.append(v)
            walk(v)
    elif isinstance(o,list):
        [walk(i) for i in o]
walk(d)
if not urls: sys.exit("no .mp3 url in API response")
print(urls[0])')"
echo "    got $(printf %.80s "$url")..."

echo "==> downloading mp3"
curl -fL -A "$UA" -o "$mp3" "$url"

echo "==> transcoding to mono 24 kHz s16le raw PCM"
ffmpeg -hide_banner -loglevel error -y -i "$mp3" \
  -ac 1 -ar 24000 -c:a pcm_s16le -f s16le "$pcm"

# Verify it is the right shape AND actually contains sound, before it is usable.
python3 - "$pcm" <<'PY'
import sys, struct, math
p = sys.argv[1]
raw = open(p, "rb").read()
n = len(raw) // 2
secs = n / 24000
samples = struct.unpack("<%dh" % n, raw[: n * 2])
peak = max(abs(s) for s in samples)
rms = math.sqrt(sum(s * s for s in samples) / n)
print(f"    {len(raw)} bytes  ~{secs:.1f}s mono@24k  peak={peak}  rms={rms:.0f}")
assert secs > 120, f"too short ({secs:.1f}s) — expected ~142s"
assert peak > 1000 and rms > 100, "audio looks silent — conversion failed"
print("    OK: correct length and non-silent")
PY

echo "==> done: ${pcm}"
