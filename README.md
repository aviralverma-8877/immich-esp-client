# Immich Frame (ESP32)

Firmware for the *ESP32 RJ9 Adapter* board (ESP32-WROOM-32, ST7789 240x320
TFT on a shared SPI bus) that shows an Immich slideshow with random transitions,
configured from a web portal.
The board's EasyEDA Pro project is in `ESP32-EasyEDA/`.

## Install from the browser

Open **https://aviralverma-8877.github.io/immich-esp-client/** in Chrome or
Edge, plug the frame in over USB and press *Connect*. No toolchain needed.

## Build and flash

```sh
pio run -e immich-frame -t upload        # first flash over USB (CH340)
pio device monitor                       # logs at 115200
```

Optional first-boot defaults go in `include/secrets.h` (copy
`include/secrets.example.h`). Anything saved in the portal overrides them.
Later updates can be uploaded from the portal (Device > Firmware update) using
`.pio/build/immich-frame/firmware.bin`.

To refresh the web installer, run `pio run -e release`. It builds without
`include/secrets.h` and writes `docs/firmware/immich-frame.bin` plus the
version in `docs/manifest.json`; commit both and GitHub Pages serves them.

## First run

1. With no WiFi saved (or if it can't join), the frame starts a setup access
   point `ImmichFrame-XXXX` with a random WPA2 password, and shows a QR code
   that joins it. The captive portal opens by itself; otherwise browse to
   http://192.168.4.1.
2. The setup page lists nearby networks and asks for the Immich server URL
   and API key. *Connect* tries the WiFi while the AP stays up, checks the
   Immich login, reports the frame's new address, then restarts.
3. On your network the portal is at the frame's IP or
   `http://immich-frame.local` (full settings are also at `/settings`).
4. Choose what to show: all photos, an album, a person, a tag or favourites,
   plus order, interval, fit/fill and orientation (portrait by default).

## Controls

| Input | Action |
| --- | --- |
| BTN1 (IO39) | next; hold 1.2 s to pause/resume |
| BTN2 (IO36) | previous |
| Touch (after calibration) | left third previous, middle pause, right third next |
| Touch (uncalibrated) | any tap = next |
| BOOT held 6 s | factory reset |
| BTN1 + BTN2 held at power-up for 3 s | factory reset |

Run *Calibrate touch* from the portal once, and again after changing orientation.

## Display colours

ST7789 modules that look identical differ in colour order and inversion. This
board's panel is BGR (the default). If colours are wrong on another panel, use
*Display colours > Show test pattern* in the portal and adjust *Colour order*
and *Inversion* until each bar matches its label; changes apply instantly.

## How it works

- `immich_client` uses `POST /api/search/random` (shuffle) or
  `POST /api/search/metadata` (newest/oldest, paged) with `albumIds`,
  `personIds`, `tagIds` or `isFavorite`, fetching 20 ids per batch.
- Each photo is the Immich *preview* rendition (`/api/assets/{id}/thumbnail?size=preview`,
  1440 px JPEG by default). `jpeg_stream` decodes it straight from the HTTP
  stream with TJpgDec, using the coarsest 1/1–1/8 scale that still covers the
  screen and then resampling to an exact fit or fill, so no image buffer is
  needed (the WROOM-32 has no PSRAM).
- The portal (`portal.cpp`, page in `portal_html.h`) runs in its own task on
  core 0; the slideshow runs in `loop()` on core 1. Requests to Immich are
  serialised so only one TLS session is open at a time.

Requirements: Immich preview format must be JPEG (Administration > Settings >
Image), which is the default. Tested against the API of Immich 3.2.
