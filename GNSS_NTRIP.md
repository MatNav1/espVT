# GNSS variants, UM982 and NTRIP

## Purpose

The firmware has two mutually exclusive GNSS product variants:

| Variant | Input | Capabilities |
| --- | --- | --- |
| RTK | UM982 3.3 V TTL UART | NMEA, RTK quality, Wi-Fi NTRIP and RTCM forwarding |
| Serial | Standard NMEA via RS-232 | Position, speed, course, GNSS fix quality and external RTK when supplied by the receiver |

Both use the same NMEA parser for `GGA` and `RMC`. The RTK variant can connect to a
phone hotspot or cab router, receive RTCM from an NTRIP caster and forward those
correction bytes to UM982 over the same UART.

## Wiring

## RTK variant wiring

Use the UM982 board's 3.3 V TTL UART only:

| UM982 | ESP32-S3 |
| --- | --- |
| TX | configured RX GPIO |
| RX | configured TX GPIO |
| GND | GND |

Do not connect an RS-232-level port directly to ESP32-S3. GPIO19/20 remain reserved
for the board CAN interface. The GNSS GPIO values are intentionally unset by default;
choose only pins confirmed as exposed and unused on the delivered LCD board.

## Basic RS-232 variant wiring

Use a MAX3232-compatible, 3.3 V RS-232 transceiver between the GPS connector and
ESP32-S3. The TTL side connects to the configured ESP UART pins and the RS-232 side
connects to the receiver's TX/RX/GND. Do not connect the ESP directly to an RS-232
connector; the voltage levels can damage it.

For a read-only NMEA GPS, GPS TX, GND and the MAX3232 path to ESP RX are sufficient.
Connect the reverse direction as well when the receiver needs serial configuration.

### Sending speed to the RS-232 variant

The RS-232 navigation profile uses `57600 bit/s, 8 data bits, no parity, 1 stop bit`
(`57600 8N1`). Each NMEA sentence must start with `$`, contain a valid checksum after
`*` and end with `CR LF` (`\r\n`). The recommended navigation profile is:

- `GGA` at 5 Hz for position, satellite count and RTK FIX/FLOAT quality.
- `VTG` at 1 Hz for course and speed over ground.
- `RMC` is optional and remains supported as a fallback for position, course and speed.

Example representing `10.0 km/h` (`5.40 kn`) and an RTK FIX:

```text
$GPGGA,120000.00,5213.0000,N,02100.0000,E,4,12,0.8,100.0,M,0.0,M,,*56\r\n
$GPVTG,90.0,T,,M,5.40,N,10.00,K,A*04\r\n
```

The talker prefix may be `GP`, `GN` or another NMEA prefix; the parser identifies the
sentence by its final three characters (`RMC` or `GGA`). The RMC status must be `A`.
GGA quality `4` means RTK FIX and `5` means RTK FLOAT.

The current firmware uses this speed in its GNSS display and section-control state. It
does not yet publish the received speed onto ISOBUS. An implement which needs GPS speed
from this device additionally requires Ground Based Speed and Machine Selected Speed
message transmission.

An RS-232 receiver can supply the position gate for future automatic sections when it
provides a current RTK FIX in its NMEA GGA messages. The RS-232 port is only the
electrical transport; the receiver may obtain corrections through its own NTRIP client,
radio modem or cellular connection. A low-cost autonomous NMEA GPS does not satisfy the
required accuracy.

## Configuration

1. Configure the UM982 output port for NMEA `GGA` and `RMC`, and RTCM input at the
   same baud rate as `ISOVT_UM982_UART_BAUD_RATE`.
2. In `idf.py menuconfig`, choose `ESP ISOBUS VT -> Optional GNSS input` and select
   either the RTK UM982 or Basic RS-232 variant.
3. Set the UART controller and GPIOs, then enter the Wi-Fi and NTRIP settings.
4. Use a phone hotspot or a cab router as the ESP32-S3 Wi-Fi uplink.

The NTRIP client sends the latest valid GGA every five seconds and forwards the RTCM
stream unchanged to UM982. The boot screen reports GNSS fix quality and NTRIP state.

For a separate verification build with the optional module compiled in, use:

```sh
idf.py -B build-ntrip -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.ntrip.defaults" build
```

The profile GPIO values `11` and `12` are build-only placeholders. Do not wire UM982
to them before verifying the Waveshare board's exposed connector and schematic.

For a separate Basic RS-232 compilation profile, use:

```sh
idf.py -B build-rs232 -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.rs232.defaults" build
```

## Safety for Section Control

No section commands are sent by this GNSS module. A later TC-GEO layer must require a
fresh position meeting the selected quality policy, a valid machine working-set and an
explicit automatic-mode command. When this firmware supplies corrections through NTRIP,
its RTCM stream must also be fresh. An RS-232 receiver can satisfy the same condition
when it has independently acquired and reports the selected RTK quality. On any missing
condition, automatic commands must stop and the implement must remain in its defined
manual-safe state.

`menuconfig` provides two quality policies for the future automatic section controller:

- `RTK FIX only` is the default and should be used for normal production work.
- `RTK FLOAT or RTK FIX` permits RTK float after validating the actual receiver,
  implement width, speed and tolerated overlap in a field test.

In both modes, position must be younger than one second. When ESP32-S3 itself provides
NTRIP, received RTCM data must additionally be younger than five seconds. A float fix is
not inherently bounded like a fixed ambiguity solution, so it should never be enabled
only because the NMEA status appears stable.

The waiting screen also shows the `SECTION GEO` safety state. `READY` means the GNSS
gate is satisfied for the selected policy; `OUTPUT LOCKED` remains intentional until a
specific implement DDOP has been validated and connected to a TC-GEO output adapter.
