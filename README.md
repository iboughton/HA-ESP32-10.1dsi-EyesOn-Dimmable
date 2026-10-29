<!-- SPDX-License-Identifier: LicenseRef-FNCL-1.1 | Copyright (c) 2026 Cpt_Kirk -->
<img src="images/BETTAOS.jpg" alt="BETTA OS Logo" width="10%" />

# BETTA HA Panel

A runtime-configurable Home Assistant wall panel for ESP32-P4 and ESP32-S3 touchscreen devices. Build your dashboard directly on the device — no YAML edits, no firmware rebuilds.

<p float="left">
  <img width="32%" alt="Page settings" src="https://github.com/user-attachments/assets/9caf6e2b-6ea9-4b76-b404-1b58da822712" />
  <img width="32%" alt="Media Player & Roborock" src="https://github.com/user-attachments/assets/c31df7b8-f7e7-461a-98af-80070eac0392" />
  <img width="32%" alt="Widget inspector" src="https://github.com/user-attachments/assets/97be77c3-0716-4641-994c-efe90c929953" />
</p>
---

## Changes in this fork

This is a modified version of [BETTA HA Panel](https://github.com/cptkirki/BETTA-HA-PANEL) by Cpt_Kirk, shared under the same [FNCL v1.1](LICENSE) license. It is built around the `panel10` hardware: a Waveshare ESP32-P4-NANO with the **10.1-DSI-TOUCH-A** display. Changes from the original:

### Wired Ethernet and PoE (`panel10`)

- The panel uses the ESP32-P4-NANO's built-in Ethernet port (IP101 PHY) as its primary network connection.
- At boot it waits up to 5 seconds for a cable link, then up to 30 seconds for a DHCP address. If Ethernet connects, Wi-Fi stays off. If there's no cable or no address, it falls back to Wi-Fi as before.
- With the board's optional PoE adapter fitted, a single Ethernet cable from a PoE switch or injector powers the panel and provides its network connection. The PoE adapter is hardware only; the firmware needs no extra settings for it.

### Screen dimming and brightness controls

- **Auto-dim:** the backlight dims to 10% after 2 minutes without a touch.
- **Wake-touch guard:** the touch that wakes a dimmed screen only restores the brightness. It isn't passed on to the button underneath, so you can't switch a light or arm the alarm by accident while waking the screen.
- **Brightness buttons:** − and + buttons at the bottom right of the navigation bar change the brightness in 10% steps (10–100%). The level is saved and restored after a reboot and when the screen wakes.
- **10.1-DSI-TOUCH-A backlight fix:** this panel's backlight controller only accepts brightness commands on register `0x96`. The original firmware wrote register `0x86`, which this panel ignores, so brightness could not be changed at all.

### Alarm panel widget (DSC PC1616 via EyezOn EnvisaLink 4)

A new **Alarm Panel** widget turns a dashboard page into a security keypad for a Home Assistant `alarm_control_panel` entity. It was built and tested with a **DSC PowerSeries PC1616** connected to Home Assistant through an **EyezOn EnvisaLink 4 (EVL-4)** module, but it works with any alarm panel Home Assistant exposes.

- **Keypad and arming:** a number pad with Disarm, Arm Away, Arm Home, Arm Night and Vacation buttons. The code is entered for each action and never stored on the panel. If Home Assistant rejects a code, the panel shows the error.
- **Status:** the current alarm state (disarmed, armed, arming, triggered and so on) is shown in colour, together with the alarm keypad's text display.
- **Zones:** up to 16 zone sensors (doors, windows, motion detectors) with their open or closed state.
- **Actions:** up to 8 switches and buttons, for example the chime switch, plus fire, police and ambulance panic buttons. Panic buttons only fire after being held for 2 seconds, so a stray tap can't trigger them.
- **History:** a log of recent alarm events, loaded from Home Assistant's logbook (24 hours by default) and updated live.
- The widget is added and configured in the web editor (**+ Alarm Panel**), where you pick the alarm entity, zones and actions.

### Other changes

- **Display stack (`panel10`):** display and touch now run on Espressif's `esp_lvgl_adapter` instead of `esp_lvgl_port`, with tear-free double-buffered drawing. The `esp_lvgl_adapter`, `esp_driver_ppa` and `esp_lcd_jd9365` components are bundled in `components/`, under their original Apache-2.0 licenses.
- **Flash layout (`panel10`):** configured for the board's 16 MB flash with a custom partition table (`partitions.panel10.csv`).
- **Code structure:** the dim timer moved out of the three panel drivers into a shared `main/drivers/display_dim.c`.
- **ESP-IDF 6.1:** all three variants build with ESP-IDF v6.1. `panel4` and `panels3` needed fixes for this:
  - LVGL pinned to 9.4 for all variants (LVGL 9.6 turns a deprecation warning into a build failure).
  - `panel4`: a patched copy of Waveshare's ESP32-P4-WIFI6-Touch-LCD-4B board package in `vendor/`, updated for IDF 6 driver changes.
  - `panels3`: the S3 RGB display driver updated for the IDF 6 colour-format API, and the P4-only display components excluded from the S3 build.
  - Each build preset now sets its chip target.
- **Firmware updates:** the web UI's OTA update looks for new firmware in this fork's GitHub Releases, not the original project's, so an update can't replace this firmware with the original. The version is `v0.8.2-eyeson.1`.

### Tested hardware

Only the `panel10` variant (with the 10.1-DSI-TOUCH-A display) has been tested on hardware. The `panel4` and `panels3` images below build cleanly and include the same dimming changes, but nobody has run them on a real device yet.

---

## Supported hardware

BETTA HA Panel ships as **three firmware variants**, one per supported device:

| Variant    | Device                                                  | Resolution | Factory image                                                                     |
|------------|---------------------------------------------------------|------------|-----------------------------------------------------------------------------------|
| `panel4`   | Waveshare **ESP32-P4-WIFI6-Touch-LCD-4B** (4")          | 720 × 720  | [betta86-ha-panel-v0.8.2-eyeson.1-panel4.factory.bin](release/betta86-ha-panel-v0.8.2-eyeson.1-panel4.factory.bin)   |
| `panel10`  | Waveshare **ESP32-P4 Module Nano + 10.1" DSI panel**    | 1280 × 800 | [betta86-ha-panel-v0.8.2-eyeson.1-panel10.factory.bin](release/betta86-ha-panel-v0.8.2-eyeson.1-panel10.factory.bin) |
| `panels3`  | Guition **ESP32-S3-4848S040** (4")                      | 480 × 480  | [betta86-ha-panel-v0.8.2-eyeson.1-panels3.factory.bin](release/betta86-ha-panel-v0.8.2-eyeson.1-panels3.factory.bin) |

All variants share the same dashboard engine, web editor, and Home Assistant integration. Pick the image that matches your board.

---

## Main features

- **Live Home Assistant link** — WebSocket connection with REST fallback for forecasts and long-poll states.
- **On-device editor** — BETTA Editor in the browser at `http://<panel-ip>`; drag-and-drop widgets, multi-page layouts, room-grouped entity picker.
- **Widget library** — sensor, button, slider, graph, light, heating, weather, up to 5 day weather forecast, media player, todo list, Roborock, energy dashboard, empty tile.
- **Advanced light control** — brightness, color temperature, RGB — exposed only when Home Assistant reports the capability.
- **Energy dashboard** — automatic grid / solar / battery / gas / water flow visualization driven by the Home Assistant energy model.
- **Graphs** — line, smoothed line, or bar-chart modes; event-rate sampling up to 4096 points with progressive decimation.
- **First-run provisioning** — `BETTA-Setup` Wi-Fi AP, guided Wi-Fi + Home Assistant setup, Quick Setup flow for a starter dashboard.
- **OTA updates** — upload an `.ota.bin` or point to an OTA URL from the web editor; no reflash required after v0.7.1.
- **Multilingual** — built-in English, German, Spanish, French; custom translation JSON upload/download.
- **Touch-friendly UX** — auto-dimming backlight after idle, pointer-capture drag/resize, stable GT911 touch startup.

---

## Getting started

1. **Download** the factory image for your board from the table above.
2. **Flash** it with any ESP32 flasher — for example the browser-based [esptool-js](https://espressif.github.io/esptool-js/):
   - Use the outer USB-C port.
   - Baud rate `115200`, flash offset `0x0`.
   - The factory image includes the ESP32-C6 network coprocessor firmware.
3. **Reboot** the device. It opens a Wi-Fi AP called `BETTA-Setup`.
4. Connect to `BETTA-Setup`, open `http://192.168.4.1`, pick your country, scan for your network and save.
5. After reboot the panel joins your LAN. Open its IP in a browser, link Home Assistant via long-lived access token, and build your first page with **Quick Setup**.

Future updates install via OTA from the editor — no cable needed.

<img width="1080" alt="image" src="https://github.com/user-attachments/assets/cda02fa3-8270-48c2-bde8-987ebf069c36" />
<img width="1080" alt="image" src="https://github.com/user-attachments/assets/5d72053d-2889-413f-bc8a-f2170ea1e889" />
<img width="1080" alt="image" src="https://github.com/user-attachments/assets/9cbd3880-07da-4e14-b81c-d3e52cfd8dad" />



---

## What's new in v0.8.2

- **ESP32-S3 support** — new `panels3` variant for the Guition ESP32-S3-4848S040 (4.8" 480×480 RGB panel, 16 MB flash, 8 MB PSRAM).
- **Three-variant release** — factory and OTA images now ship for `panel4`, `panel10`, and `panels3`.
- **MDI weather icons on S3** — clean Material Design Icon weather display on the S3 panel.
- **Release tooling** — `make_factory_bin.ps1` extended; `-Variant both` now packages all three variants in one run.

Full history: [release-notes.md](release-notes.md).

---

## Building from source

Prerequisites: **ESP-IDF v6.1**, Python 3.11+, the Smart86 / Waveshare BSP components (pulled automatically via the component manager).

```powershell
# Pick a variant preset (configure once, then build)
cmake --preset panel4;   cmake --build build-panel4
cmake --preset panel10;  cmake --build build-panel10
cmake --preset panels3;  cmake --build build-panels3

# Package release images (factory + OTA) for one or both variants
pwsh tools/make_factory_bin.ps1 -Variant both
```

Build artifacts land in `release/` and `release/ota/`. Previous versions are moved to `release/archive/`.

---

## Editor preview


<p>
  <img width="49%" alt="Energy dashboard" src="https://github.com/user-attachments/assets/96d51c1d-743a-4c7a-b2c6-29f2cff1e41f" />
  <img width="49%" alt="languages" src="https://github.com/user-attachments/assets/93702c1a-994c-4390-bfbc-6b4eb6b9d779" />
</p>
<p>
  <img width="49%" alt="theme editor" src="https://github.com/user-attachments/assets/2c016e94-bd7e-4c4e-a4e5-2826e6ba6269" />
  <img width="49%" alt="OTA firmware" alt="image" src="https://github.com/user-attachments/assets/f655f102-47dc-4881-aed1-3e6a8cd451a4" />

</p>

---

## License

Source released under [LicenseRef-FNCL-1.1](LICENSE). See [release-notes.md](release-notes.md) for per-version changes.
