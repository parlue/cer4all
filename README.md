# cer4all

USB-to-Bluetooth Low Energy bridge for Certabo, Tabutronic Spectrum and Sentio chessboards, running on ESP32-S3.

**Development firmware: 0.1.0-dev.** Both hardware targets build successfully and portable software tests pass. Testing with physical chessboards is still pending.

## Web flasher

**[Install firmware](https://parlue.github.io/cer4all/)** using Chrome or Edge on a computer. Choose the package that matches your hardware:

| Target | Hardware | Flash |
| --- | --- | --- |
| `cer4all_s3mini` | Waveshare ESP32-S3-Zero / S3FH4R2 | 4 MB |
| `cer4all_s3wroom` | ESP32-S3-WROOM-1-N16R8 | 16 MB |

Disconnect the chessboard before flashing. On the Mini, hold BOOT and briefly press RESET if necessary to enter download mode. Reset the module after installation. On the WROOM, use the USB-UART connector for flashing and power; use the native USB-OTG connector for the chessboard.

## Connecting a chessboard

Connect the board through a suitable externally powered OTG-Y cable. Native USB uses GPIO19 (D-) and GPIO20 (D+). The firmware does not control a VBUS power switch. PSRAM is disabled on both targets.

In your chess application, connect to the BLE device named **Certabo**. This name is shared by both board paths. Piece calibration and piece-type assignment remain the application's responsibility.

## Board identification

The bridge queries the Silicon Labs USB chip with `GET_PARTNUM` before selecting the LED path. It does not identify boards from piece IDs, ID prefixes, MAC addresses or BLE packet lengths, and sends no LED identification probe.

| USB chip response | Selected path |
| --- | --- |
| CP2103 (`0x03`) | Certabo: native 8-byte LED masks |
| CP2102N (`0x20`, `0x21`, `0x22`) | Spectrum: RGB LEDs |
| Other response or failed query | Unknown: native forwarding, no LED conversion |
| Complete position record containing 8 values | Sentio: overrides the initial chip hint |

The chip mapping targets the boards used for this project. It is not a guarantee for every future hardware revision. USB VID/PID alone do not distinguish these chip variants.

## Protocol and forwarding

- USB serial: 38400 baud, 8N1, DTR/RTS enabled, 64-byte IN transfers.
- Nordic UART Service: `6e400001-b5a3-f393-e0a9-e50e24dcca9e`.
- Write characteristic: `6e400002-b5a3-f393-e0a9-e50e24dcca9e`, with or without response.
- Notify characteristic: `6e400003-b5a3-f393-e0a9-e50e24dcca9e`.
- Preferred BLE MTU: 256; smaller negotiated MTUs are supported through fragmentation.
- No pairing or bonding is required.
- One chess application connection plus an optional diagnostic connection.
- Ordered, bounded queues: 32 KiB USB-to-BLE and 16 KiB application-to-USB. Queue exhaustion causes an explicit error and disconnect rather than silent truncation.

Certabo, Sentio and unknown boards receive native LED bytes unchanged. Spectrum accepts native 247-byte RGB frames and converts standard 8-byte masks to the four corner LEDs of each square. Converted masks use blue at 80% brightness. Identical completed Spectrum LED outputs are suppressed without a timed delay. There is no move interpretation, blink scheduling or field accumulation in the bridge.

A 247-byte RGB frame takes approximately 64 ms over the 38400-baud serial link. Sustained rates above roughly 15 frames per second can therefore create a backlog.

### Spectrum LED input modes

In `AUTO`, a single 8-byte BLE write is treated as a monochrome mask; otherwise `FF55` starts an RGB frame. Arbitrary fragmentation can be ambiguous because monochrome masks have no frame signature. For such clients, select `LEDS MONO` or `LEDS RGB` in diagnostics before starting the application session. RGB-to-monochrome conversion is not implemented.

### Runtime noise filtering

No piece IDs or prefix lists are built in. The Spectrum filter learns stable IDs in RAM after three consistent observations without duplicate candidates on another square. Known IDs and empty readings then pass immediately. Unstable unknown values can hold the previous square contents for at most three observations. Learned IDs are cleared when USB reconnects.

Stable sensor noise can resemble a real new piece; filtering cannot reliably distinguish every such case. The filter can be disabled for comparison tests.

## Building

Install PlatformIO Core, then run from the project directory:

```sh
pio run -j 2 -e cer4all_s3mini
pio run -j 2 -e cer4all_s3wroom
```

The project uses PlatformIO `espressif32 6.8.1`, ESP-IDF `5.3.0`, Espressif CP210x host driver `2.2.0` and CDC-ACM host driver `2.1.0`. Component versions are locked in `dependencies.lock`.

On Windows with Visual Studio 2022 Build Tools installed, run the portable tests and then package the successful firmware builds:

```bat
tools\test_core.cmd
python tools\package.py
```

The test runner generates a local `TEST_RESULT.json`. Packaging checks its source hashes and uses the actual IDF flash offsets. Generated development reports remain local and are excluded from Git. The test runner currently expects the standard Visual Studio Build Tools installation path.

## Diagnostics

Open **[BLE diagnostics](https://parlue.github.io/cer4all/debug.html)** in a browser supporting Web Bluetooth. It provides board/chip status, connection and traffic counters, live logs and text-file export through a separate BLE service.

- Service: `ce4c0000-5887-4d42-9888-31489f8ca144`
- Status/control: `ce4c0001-5887-4d42-9888-31489f8ca144`
- Log notifications: `ce4c0002-5887-4d42-9888-31489f8ca144`

Commands: `STATUS`, `FILTER 0`, `FILTER 1`, `LEDS AUTO`, `LEDS MONO`, `LEDS RGB`. Change settings before the application's NUS notifications are active. Settings are RAM-only and reset on reboot. Diagnostics do not archive raw piece IDs.

To serve the web tools locally:

```sh
python -m http.server 8934 --bind 127.0.0.1 --directory docs
```

Then open `http://localhost:8934`. Remote hosting requires HTTPS for browser device access.

## Repository contents

`src/` and `include/` contain the firmware, `test/` contains portable C++ tests, and `tools/` contains test and packaging scripts. `docs/` hosts the web flasher, diagnostics and firmware packages. Required third-party license notices are in `third_party/LICENSES/`.

This is a separate project from eChessGw and eChessTactics.
