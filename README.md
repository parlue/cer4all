# cer4all

USB-to-Bluetooth Low Energy bridge for Certabo, Tabutronic Spectrum and Sentio chessboards, running on ESP32-S3.

**Development firmware: 0.1.4-dev.** Requests BLE Data Length Extension (251 octets) and 2M PHY, retains the 7.5–15 ms interval request and logs negotiated parameters plus queue performance every five seconds. Board bytes and repeated LED commands remain unchanged. Host tests/builds do not establish hardware performance; Spectrum/WROOM retesting is required. See [PERFORMANCE.md](PERFORMANCE.md).

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
- One chess application connection; the existing eChessGateway app is used separately for log download.
- Ordered, bounded queues: 32 KiB USB-to-BLE and 16 KiB application-to-USB. Queue exhaustion causes an explicit error and disconnect rather than silent truncation.

Certabo, Sentio and unknown boards receive native LED bytes unchanged. Spectrum accepts native 247-byte RGB frames and converts standard 8-byte masks to the four corner LEDs of each square. Converted masks use blue at 80% brightness. Every completed Spectrum LED command is forwarded, including identical repeats and keep-alives. There is no move interpretation, blink scheduling or field accumulation in the bridge.

A 247-byte RGB frame takes approximately 64 ms over the 38400-baud serial link. Sustained rates above roughly 15 frames per second can therefore create a backlog.

### Spectrum LED input

A single 8-byte BLE write is treated as a monochrome mask; otherwise `FF55` starts an RGB frame. Arbitrary fragmentation can be ambiguous because monochrome masks have no frame signature. Standard complete RGB frames and 20-byte RGB fragments are supported. No runtime configuration interface is provided. RGB-to-monochrome conversion is not implemented.

### Transparent board data

USB board data is forwarded byte-for-byte for every model, including arbitrary piece IDs, duplicate IDs, sensor-noise readings, acknowledgements and whitespace. No piece learning, temporal filtering, replacement with empty fields or retention of previous positions occurs in the interface. The chess application remains responsible for calibration and handling sensor noise, just as with direct USB. Passive Sentio detection observes the stream without changing it.

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

## Logging: existing eChessGateway workflow

Use **Gateway Log** in the existing **eChessboards Android app**, exactly as with eChessGw. No browser diagnostics, local HTTP server or new configuration service is needed.

1. Disconnect the chess application from cer4all.
2. Open Gateway Log in eChessboards and leave that screen open. Restart cer4all if needed.
3. cer4all finds the app's `eChessGateway` advertisement, connects to its existing log service and uploads the stored log.
4. Use the app's existing recording controls to enable logging. Disconnect the log session, reproduce the issue, then return to Gateway Log to download and save the result.

The module is the BLE client; the Android app is the GATT server. UUIDs, framing and commands match eChessGw:

- Service: `e5c40000-6c4f-4a3b-9d2e-6b1a7f0c0001`
- Log writes: `e5c40001-6c4f-4a3b-9d2e-6b1a7f0c0001`
- Command notifications: `e5c40002-6c4f-4a3b-9d2e-6b1a7f0c0001`
- Upload: `GWLOG1 <bytes> rec=<0|1> fw=<version> up=<ms>`, log bytes, then `\n#GWLOG-END\n`.
- Commands: `REC1`, `REC0`, `CLR`, `BYE`; successful recording/clear commands receive `#OK <command>`.

The latest log remains in a 12 KiB RAM ring. Recording is off by default; enabling it persists the setting and writes to two rotating flash files, approximately 48 KiB each. Recording switches off automatically after three subsequent recording boots or two hours. Log scanning is paused while a chess application is connected. File writes and log-upload pacing run in the low-priority logging task, not in the USB/BLE forwarding callbacks.

This firmware does not record chess games or implement the gateway's optional PGN commands. No raw piece IDs are archived. The existing app workflow still needs testing on physical hardware.

## Repository contents

`src/` and `include/` contain the firmware, `test/` contains portable C++ tests, and `tools/` contains test and packaging scripts. `docs/` hosts the web flasher and firmware packages. Required third-party license notices are in `third_party/LICENSES/`.

This is a separate project from eChessGw and eChessTactics.
