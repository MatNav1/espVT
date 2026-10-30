# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

espVT is an experimental (not ISO 11783 certified) ISOBUS Virtual Terminal server (ISO 11783-6) for the Waveshare ESP32-S3-Touch-LCD-7B (1024x600 RGB LCD, GT911 touch, TWAI CAN). It is built on ESP-IDF (verified on 5.5) with a vendored, locally modified AgIsoStack++. User-facing docs (README.md, BRINGUP.md, GNSS_NTRIP.md, auxn_buttons_esp32/README.md) are written in Polish. Code and comments are in English.

The repo has two firmware projects:
- Root: the VT terminal (ESP-IDF, component in `main/`).
- `auxn_buttons_esp32/`: a separate PlatformIO (Arduino + ESP-IDF) AUX-N Type 2 button panel for a classic ESP32-WROOM-32U. It links the same `third_party/AgIsoStack-plus-plus` through a symlink.

## Build

```sh
idf.py set-target esp32s3
idf.py build
idf.py -p PORT flash monitor
idf.py menuconfig          # "ESP ISOBUS VT" menu: Kconfig in main/Kconfig.projbuild
```

The `main` component build requires the macOS `sips` tool (`find_program(... REQUIRED)`). It converts `logo.png` to BMP, and `tools/bmp_to_rgb565.py` then produces the generated `logo_rgb565.hpp` header. The build fails on Linux unless you provide a `sips` substitute.

Build variants are separate build dirs plus layered sdkconfig defaults. `sdkconfig*` (except `*.defaults`) and `build*/` are gitignored:

```sh
# RS-232 NMEA variant (release variant)
idf.py -B build-rs232 -D SDKCONFIG="$PWD/sdkconfig.rs232" \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.rs232.defaults" build
cmake --build build-rs232 --target merge-bin   # -> build-rs232/merged-binary.bin, flash at 0x0
```

`sdkconfig.ntrip.defaults` selects the UM982 RTK + NTRIP variant. It turns blobs back on (`CONFIG_APP_NO_BLOBS=n`) because it needs Wi-Fi, while the default config uses `CONFIG_APP_NO_BLOBS=y`. Wi-Fi/NTRIP credentials go into menuconfig only. Never commit a configured `sdkconfig`.

`main/vt_font_roboto_mono.bin` is a committed generated asset, embedded through `EMBED_FILES`. To regenerate it, use `tools/generate_vt_font.py` (needs Pillow) with `assets/fonts/RobotoMono-Regular.ttf`.

AUX-N panel: `cd auxn_buttons_esp32 && pio run -t upload`. Button count and pins come only from `config/buttons.json`. The pre-build script `tools/generate_config.py` regenerates `src/generated/*`, which includes the AUX-N object pool.

## Host tests (no hardware)

The tests in `tools/` compile against a host `c++` (C++17):

```sh
python3 tools/test_rectangle_suppression.py   # draw_outline() from vt_scene_renderer.cpp
python3 tools/test_render_lock_order.py        # render_working_set() deadlock regression from isobus_terminal_server.cpp
python3 tools/test_rcteensy_hydraulic_hold.py <path/to/RCteensy source>   # tests code from an external RCteensy project
c++ -std=c++17 -pthread -Ithird_party/AgIsoStack-plus-plus/utility/include \
  tools/test_can_queue_producers.cpp -o /tmp/q && /tmp/q   # multi-producer LockFreeQueue
```

The Python tests don't link the firmware. They **extract function bodies from the real source by string anchors** and compile them with stubs. For example, `test_render_lock_order.py` slices from `bool render_working_set(` to `isobus::NAME make_terminal_name()`, and `test_rectangle_suppression.py` slices from `void draw_outline(` to `std::string clipped_text(`. If you rename, move or reorder those functions or their neighbours, update the anchors in the tests.

`tools/inspect_vt_pool.cpp` is a host utility that dumps the object types in an IOP file through AgIsoStack++'s working-set parser.

## Architecture

Startup (`main/app_main.cpp`) runs in this order, and the order matters:
1. NVS
2. I2C and the CH32V003 IO expander (`board_support`), with EXIO5 high to select CAN instead of USB
3. TWAI/CAN start: this must claim GPIO19/20 before native USB keeps them
4. SPIFFS mount for `object_pool_store`: this must finish before RGB LCD scanout starts reading PSRAM, because SPIFFS disables cache
5. LCD/status view, touch, local VT menu/render task, audio, GNSS and the section-control gate

The VT server itself starts in a `vt_bus_wait` task once the bus is active. AgIsoStack++ uses `std::thread` (pthreads). `app_main` sets the pthread defaults to a 24 KB stack pinned to core 1.

Main modules:
- `isobus_terminal_server.cpp` is the core. It subclasses the AgIsoStack++ VT server, handles object-pool store/load callbacks into `object_pool_store` (SPIFFS, keyed by client NAME + version label), and dispatches events to the view. It also runs the `vt_scene` render task (core 0) and maps touch to VT messages (`on_touch_pressed/released`), including the local terminal menu actions. Render requests come from CAN threads and from touch. Lock ordering between `render_request_mutex` and the server's working-set list lock has caused deadlocks before (see the lock-order test).
- `vt_scene_renderer.cpp` software-renders VT objects (masks, containers, soft keys, buttons, inputs, output string/number/line/rectangle, …) into `lcd_panel` primitives. It builds the touch hit map, and owns the local terminal menu, input editors and persisted `TerminalSettings`. The logical VT area is 480x480 (scaled to 600x600) or 600x600 at 1:1. This is reported in VT Get Hardware, and changing it erases stored pools.
- `lcd_panel.cpp` is the RGB565 1024x600 driver with double framebuffers in PSRAM, clipping, and text drawing with the embedded font and ISO 8859 encodings.
- `lvgl_terminal_view.cpp` (`vt_view::`) is the status/diagnostic and waiting screen shown before a working set is active. Despite the name, it is not LVGL-based.
- `aux_n_manager.cpp` handles AUX-N Type 2 (and legacy Type 1): it discovers input devices, keeps the function catalog, and does learning/assignment.
- `can_monitor.cpp` sets up the TWAI hardware interface and monitors bus/machine status.
- `gnss_um982.cpp` reads NMEA GGA/RMC from a UM982 UART or an RS-232 (MAX3232) port, plus an NTRIP client that feeds RTCM to the UM982. `section_control.cpp` gates section automation on fresh GNSS fix quality (RTK FIX by default).
- `vt_audio.cpp` drives an I2S MAX98357A amp for alarm-mask/Control Audio Signal sounds. It shares GPIO12/13 with the SD card.
- `board_pins.hpp` holds all hardware pin constants.

Most AgIsoStack-dependent code is wrapped in `#if ISOVT_ENABLE_AGISOSTACK` (Kconfig `ISOVT_ENABLE_AGISOSTACK`). Keep the `#else` stubs compiling.

## Vendored AgIsoStack++

`third_party/AgIsoStack-plus-plus` is a modified copy, not a submodule. `ESPVT_UPSTREAM.md` records the upstream base commit and summarizes the local changes: TWAI reliability, VT pool processing, transport timing, and multi-producer CAN TX queue. Its `isobus` target gets `ISOVT_ESP_POOL_THREADS=1` from `main/CMakeLists.txt`. Changes to the stack belong there. If you add a local modification, note it in `ESPVT_UPSTREAM.md`.

## Hardware cautions from docs

- Set `ISOVT_MANUFACTURER_CODE` and `ISOVT_IDENTITY_NUMBER` in menuconfig before connecting to a real bus. The defaults are placeholders.
- USB and CAN share GPIO19/20. Flash through the board's UART/USB port or switch EXIO5.
- The CAN bus runs at 250 kbit/s.
