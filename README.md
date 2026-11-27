# Immich Frame (ESP32)

Firmware for the *ESP32 RJ9 Adapter* board (ESP32-WROOM-32, ST7789 240x320
TFT on a shared SPI bus) that shows an Immich slideshow with random transitions,
configured from a web portal.
The board's EasyEDA Pro project is in `ESP32-EasyEDA/`.

Also builds for the Hosyond / LCDWiki 3.5" ESP32-32E display (E32R35T:
ST7796U 320x480, XPT2046 touch): `pio run -e e32r35t -t upload`. Board pins
are in `src/board.h`. That board has no user buttons, so control it by touch
or from the portal; BOOT switches it online and off (see below). A 320x480
off-screen buffer doesn't fit in RAM without PSRAM, so photos draw straight
to the panel, with no transitions or prefetch. Its RGB LED is not used.

## Battery saving with the SD card (E32R35T)

With an **album** selected and a microSD card in the slot, the frame stores
the album on the card and plays it from there:

- The first time the frame sees a card (or a card it can't read, such as
  exFAT), it **formats it FAT32, erasing everything on it**. Portal >
  *Power* > *Format SD card* does the same on demand.
- Each photo is downloaded once, scaled to the screen and stored as a raw
  frame (300 KB), by a background task while the slideshow plays what is
  already stored. Photos are only downloaded while WiFi is on.
- **Power saver**: once every photo is stored (and the portal has been quiet
  for a minute), or when you press BOOT while the frame is online, WiFi turns
  off and the chip deep-sleeps between photos. It wakes only to show the next
  photo from the card, about 1-2 s awake each time. The photo stays on screen
  while it sleeps: the panel is never reset, and the ULP co-processor keeps
  the backlight at the chosen brightness.
- In power saver, **press BOOT** to come back online (the screen shows the
  address and battery). The frame goes back to power saver after the portal
  has been unused for 3 minutes, or at the next BOOT press. If the saved WiFi
  can't be found, it opens the setup network instead. Touch still works:
  left previous, middle pause, right next.
- **Double-tap BOOT** to show the battery level.
- **Hold BOOT for 2 s** to shut down: the screen and backlight go off.
  **Press BOOT once** to start again.
- Below 5% (not charging) the frame shows a charge-me battery and shuts
  down. Pressing BOOT then only shows that screen again until the charger is
  connected.
- The frame checks the album for new or removed photos at each start and, if
  set, every few hours (*Power* > *Check the album for changes*).

Switch it off with *Keep the album on the SD card and switch WiFi off*. Other
sources (all photos, people, tags, favourites) always stream over WiFi. The
backlight draws the most current, so lowering *Screen brightness* matters
most for battery life.

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
   *Now showing* has a preview of the photo on the frame, linked to Immich.
   The preview dims while the next one loads, and the Network row shows
   the access point (BSSID) the frame is on.
   The portal serves one device at a time: another device gets a "busy"
   page until the first has been idle (or its tab hidden) for 30 s.
4. Choose what to show: all photos, an album, a person, a tag or favourites,
   plus order, interval, fit/fill and orientation (portrait by default).

## Controls

| Input | Action |
| --- | --- |
| BTN1 (IO39) | previous |
| BTN2 (IO36) | next; hold 1.2 s to pause/resume |
| Touch (after calibration) | left third previous, middle pause, right third next |
| Touch (uncalibrated) | any tap = next |
| Portal > Device > *Re-run setup* | restart into the setup network once, keeping settings (e.g. to change WiFi) |
| BOOT press | power saver: go online and show the address; online: power saver |
| BOOT double-tap | show the battery level |
| BOOT held 2 s | shut down; press BOOT once to start |
| BTN1 + BTN2 held at power-up for 3 s | factory reset (RJ9 board; otherwise Portal > Device) |

The two buttons can be swapped under *Slideshow > Buttons* in the portal; the
pause hold always follows the next button. When a photo you asked for still
has to download, *Loading next photo...* shows at the top until it appears.

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
- The portal's preview is Immich's small *thumbnail* rendition of the asset
  id it asks for, relayed by the frame (`/api/thumb?id=`) because the browser
  has no API key. While the portal waits for the Immich connection, the
  background download of the next photo steps aside and resumes later.
- WiFi joins the strongest AP with the saved SSID (all-channel scan; the
  ESP32 default fast scan takes the first one heard), which matters with
  repeaters or mesh nodes. Every 5 min, below -70 dBm, it rescans and
  reconnects if an AP with that SSID is at least 8 dB stronger.
- The portal (`portal.cpp`, page in `portal_html.h`) runs in its own task on
  core 0; the slideshow runs in `loop()` on core 1. Requests to Immich are
  serialised so only one TLS session is open at a time.
- Responses are read with `PatientStream`, which sleeps while waiting for
  data. Arduino's `Stream::timedRead()` busy-waits instead, and on a slow
  connection that kept the portal task hogging core 0 until the task watchdog
  reset the chip (seen with the portal open in several tabs).

Requirements: Immich preview format must be JPEG (Administration > Settings >
Image), which is the default. Tested against the API of Immich 3.2.
