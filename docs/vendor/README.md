# Vendor reference material

Third-party files kept for reference. **Nothing here is built by this repo** —
they are pinned copies of upstream material that is awkward to re-obtain.

| File | Origin |
| ---- | ------ |
| `config.h`, `config.json`, `toy_ai_core_c3_1.28.cc` | `main/boards/toy-ai-core-c3-1.28/` from Spotpear's `xiaozhi-esp32-2.0.3.zip` |
| `LICENSE.xiaozhi` | MIT license shipped with that zip |
| `Toy-AI-Core-C3-MINI-schematic.pdf` | Spotpear wiki — **not tracked in git**, see below |

## Why these are here

The `toy-ai-core-c3-1.28` board directory does **not** exist in upstream
[78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) or in any public fork.
It ships only inside Spotpear's own 373 MB source zip. Re-fetching means another
373 MB download from a vendor CDN that may not stay up — hence the 11 KB of
source that actually matters is pinned here.

The `.cc` is the useful part beyond the pin numbers: it shows how stock firmware
initialises the ES8311 and the GC9A01 panel.

## License

xiaozhi-esp32 is MIT, Copyright (c) 2025 Shenzhen Xinzhi Future Technology Co.,
Ltd. and project contributors — see `LICENSE.xiaozhi`. The source files retain
that license, not this repo's.

The schematic PDF is Spotpear's proprietary document, not MIT. It is **excluded
from git** (`docs/vendor/*.pdf` in `.gitignore`) — this repo does not redistribute
it. Download it yourself from the URL below; the parts list derived from it lives
in [../board.md](../board.md).

Note it does still exist in this repo's history, in commits `413c3ac`..`96603cf`,
from before that decision.

## Upstream URLs

```
https://cdn.static.spotpear.com/uploads/picture/learn/ESP32/ESP32-C3-MINI/xiaozhi-esp32-2.0.3.zip
https://cdn.static.spotpear.com/uploads/picture/learn/ESP32/ESP32-C3-MINI/Toy-AI-Core-C3-MINI.pdf
https://spotpear.com/wiki/ESP32-mini-C3-AI-Toy-AI-Core-DeepSeek-XiaoZhi-DouBao-1.54-1.28-0.71-inch-LCD.html
```
