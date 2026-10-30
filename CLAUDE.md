# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

espVT is an experimental (not ISO 11783 certified) ISOBUS Virtual Terminal server (ISO 11783-6). Boards: the Waveshare ESP32-S3-Touch-LCD-7B (1024x600 RGB LCD, GT911 touch, TWAI CAN) and, experimentally, the Guition JC8012P4A1C_I_W_Y (ESP32-P4, 800x1280 MIPI-DSI JD9365 used as 1280x800 landscape, GSL3680 touch, external CAN transceiver). It is built on ESP-IDF 6.1 with a vendored, locally modified AgIsoStack++. User-facing docs (README.md with an English translation in README.en.md, BRINGUP.md, GNSS_NTRIP.md, auxn_buttons_esp32/README.md) are written in Polish. Code and comments are in English.

The project is GPL-3.0-or-later (earlier MIT notice kept in `LICENSES/MIT-espVT.txt`); the switch came with the GPL-2.0-or-later Silead code in `components/esp_lcd_touch_gsl3680`.

The repo has two firmware projects:
- Root: the VT terminal (ESP-IDF, component in `main/`).
- `auxn_buttons_esp32/`: a separate PlatformIO (Arduino + ESP-IDF) AUX-N Type 2 button panel for a classic ESP32-WROOM-32U. It links the same `third_party/AgIsoStack-plus-plus` through a symlink.

## Build

`./make_espVT.sh` is the build entry point. It imports the ESP-IDF environment from `IDF_ACTIVATE` (default `~/.espressif/tools/activate_idf_v6.1.sh`) when `idf.py` is not on PATH:

```sh
./make_espVT.sh                          # all targets -> delivery/espVT-<git describe>-<target>.bin (merged, flash at 0x0)
./make_espVT.sh JC8012P4 [force]         # one target; force deletes build_<target>/ first
./make_espVT.sh flash JC8012P4 /dev/ttyACM5
./make_espVT.sh menuconfig Waveshare7B_NTRIP   # "ESP ISOBUS VT" menu: Kconfig in main/Kconfig.projbuild
```

Targets: `Waveshare7B`, `Waveshare7B_RS232` (release), `Waveshare7B_NTRIP` (esp32s3) and `JC8012P4` (esp32p4); `default`/`rs232`/`ntrip`/`p4` are aliases. Configuration is layered like in logiBUS: `sdkconfig.defaults` (shared) + `sdkconfig.defaults.<chip>` + `sdkconfig.defaults.<Board>.<chip>` + optional `sdkconfig.defaults.rs232`/`.ntrip`. The top-level `CMakeLists.txt` puts each `sdkconfig` into its `build_<target>/` directory and uses `dependencies.lock.<chip>` (committed).

ESP-IDF 6.x specifics: `sdkconfig.defaults.esp32s3` sets the S3 flash HPM choices explicitly; `sdkconfig.defaults` suppresses the deprecation warning for the legacy TWAI driver (`driver/twai.h`), which `can_monitor.cpp` and the AgIsoStack++ `twai_plugin.cpp` still use. `sdkconfig.defaults.esp32p4` lowers the bootloader log level so the P4 bootloader fits below the partition table at 0x8000.

`main/CMakeLists.txt` picks board sources from Kconfig, but component `REQUIRES` must depend on `IDF_TARGET`: requirements are expanded before sdkconfig is loaded. Registry components (`esp_lcd_jd9365`, `esp_lcd_touch`) are P4-only via `rules` in the manifests, and the top-level `CMakeLists.txt` excludes `esp_lcd_touch_gsl3680` for other chips.

The boot logo is generated at build time: `tools/bmp_to_rgb565.py` reads `logo.png` (stdlib PNG decoder) and writes `logo_rgb565.hpp` into the build dir.

`sdkconfig.defaults.ntrip` selects the UM982 RTK + NTRIP variant. It turns blobs back on (`CONFIG_APP_NO_BLOBS=n`) because it needs Wi-Fi, while the default config uses `CONFIG_APP_NO_BLOBS=y`. Wi-Fi/NTRIP credentials go into menuconfig only. Never commit a configured `sdkconfig`.

Text is rendered at runtime: `assets/fonts/RobotoMono-Regular.ttf` is embedded through `EMBED_FILES` and rasterized by `main/vt_font.cpp` with `third_party/stb/stb_truetype.h` (implementation in `main/stb_truetype_impl.c`) at the exact physical cell size, cached in PSRAM (2 MB budget). Cells are stretched to the requested width, so all ISO 11783-6 font sizes including the square ones render natively. `lcd_panel::draw_text(x, y, scale, …)` uses 6x10 cells per scale step positioned so the capitals cover the old 5x7 box.

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

Board selection is the Kconfig choice `ISOVT_BOARD_*`. Board-specific code sits behind small interfaces: `board_support.hpp` (`board_support_waveshare7b.cpp` / `board_support_jc8012p4.cpp`), `touch.hpp` (`touch_gt911.cpp` / `touch_gsl3680.cpp`) and `lcd_backend.hpp` (`lcd_backend_rgb.cpp` / `lcd_backend_dsi.cpp`). `board_pins.hpp` holds the constants per board; CAN pins come from Kconfig.

Startup (`main/app_main.cpp`) runs in this order, and the order matters:
1. NVS
2. I2C and, on the Waveshare board, the CH32V003 IO expander (`board_support`), with EXIO5 high to select CAN instead of USB
3. TWAI/CAN start: this must claim GPIO19/20 before native USB keeps them
4. SPIFFS mount for `object_pool_store`: this must finish before RGB LCD scanout starts reading PSRAM, because SPIFFS disables cache
5. LCD/status view, touch, local VT menu/render task, audio, GNSS and the section-control gate

The VT server itself starts in a `vt_bus_wait` task once the bus is active. AgIsoStack++ uses `std::thread` (pthreads). `app_main` sets the pthread defaults to a 24 KB stack pinned to core 1.

Main modules:
- `isobus_terminal_server.cpp` is the core. It subclasses the AgIsoStack++ VT server, handles object-pool store/load callbacks into `object_pool_store` (SPIFFS, keyed by client NAME + version label), and dispatches events to the view. It also runs the `vt_scene` render task (core 0) and maps touch to VT messages (`on_touch_pressed/released`), including the local terminal menu actions. Render requests come from CAN threads and from touch. Lock ordering between `render_request_mutex` and the server's working-set list lock has caused deadlocks before (see the lock-order test).
- `vt_scene_renderer.cpp` software-renders VT objects (masks, containers, soft keys, buttons, inputs, output string/number/line/rectangle, …) into `lcd_panel` primitives. It builds the touch hit map, and owns the local terminal menu, input editors and persisted `TerminalSettings`. The logical VT area is 480x480 or 600x600 and is scaled to the full LCD height (`current_layout()`); the working-set bar narrows so the 83 px MENU strip still fits. The data mask size is reported in VT Get Hardware, and changing it erases stored pools.
- `lcd_panel.cpp` draws RGB565 with clipping, view transforms and the embedded font (ISO 8859 encodings) into the buffer `lcd_backend` provides. Menu and status screens are laid out for 1024x600: they call `lcd_panel::use_ui_area()`, which offsets all coordinates into a centered `board::UI_WIDTH x UI_HEIGHT` window on larger LCDs. Touch targets are stored in LCD coordinates, so code that adds targets in UI-area coordinates adds `lcd_panel::area_x()/area_y()`.
- `lcd_backend_rgb.cpp` (S3): RGB panel with two PSRAM framebuffers and bounce buffers. `lcd_backend_dsi.cpp` (P4): one landscape draw buffer, rotated by the PPA into one of two portrait DPI framebuffers on `present()`. The JD9365 init sequence (`jc8012_lcd_init.c`) and the DSI timing (2 lanes, 1500 Mbps, 70 MHz DPI) come from the vendor demo verified on the board. `touch_gsl3680.cpp` maps the portrait touch coordinates to match the PPA rotation.
- `lvgl_terminal_view.cpp` (`vt_view::`) is the status/diagnostic and waiting screen shown before a working set is active. Despite the name, it is not LVGL-based.
- `aux_n_manager.cpp` handles AUX-N Type 2 (and legacy Type 1): it discovers input devices, keeps the function catalog, and does learning/assignment.
- `can_monitor.cpp` sets up the TWAI hardware interface and monitors bus/machine status.
- `gnss_um982.cpp` reads NMEA GGA/RMC from a UM982 UART or an RS-232 (MAX3232) port, plus an NTRIP client that feeds RTCM to the UM982. `section_control.cpp` gates section automation on fresh GNSS fix quality (RTK FIX by default).
- `vt_audio.cpp` drives an I2S MAX98357A amp for alarm-mask/Control Audio Signal sounds. It shares GPIO12/13 with the SD card.

Most AgIsoStack-dependent code is wrapped in `#if ISOVT_ENABLE_AGISOSTACK` (Kconfig `ISOVT_ENABLE_AGISOSTACK`). Keep the `#else` stubs compiling.

## Vendored AgIsoStack++

`third_party/AgIsoStack-plus-plus` is a modified copy, not a submodule. `ESPVT_UPSTREAM.md` records the upstream base commit and summarizes the local changes: TWAI reliability, VT pool processing, transport timing, and multi-producer CAN TX queue. Its `isobus` target gets `ISOVT_ESP_POOL_THREADS=1` from `main/CMakeLists.txt`. Changes to the stack belong there. If you add a local modification, note it in `ESPVT_UPSTREAM.md`.

## Hardware cautions from docs

- Set `ISOVT_MANUFACTURER_CODE` and `ISOVT_IDENTITY_NUMBER` in menuconfig before connecting to a real bus. The defaults are placeholders.
- Waveshare: USB and CAN share GPIO19/20. Flash through the board's UART/USB port or switch EXIO5.
- JC8012: no CAN transceiver on the board; the default external transceiver pins are TX GPIO4 / RX GPIO5 on FPC3. Serial port on the development machine: `/dev/ttyACM5` (ttyACM0–4 are other devices).
- The CAN bus runs at 250 kbit/s.
