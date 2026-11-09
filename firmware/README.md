# ESP-IDF firmware - XIAO ESP32S3 Sense / OV2640

This profile assumes the **Sense** camera expansion with an OV2640 using Seeed's standard connector and pinout. A XIAO without that expansion, or with a camera connected through separate wires, needs its own profile; do not flash assuming a different pinout.

## Hardware and parameters

| Item | Configuration |
|---|---|
| PlatformIO board | `seeed_xiao_esp32s3` |
| Flash | 8 MB, DIO |
| PSRAM | 8 MB, octal, 80 MHz |
| Sensor | OV2640, native JPEG with a fixed monochrome effect |
| Resolution | 240 x 240, following the OpenIris profile |
| XCLK | 20 MHz |
| JPEG quality | 10 (driver scale 0-63; lower means higher quality) |
| Maximum FPS | 90, also limited by the PC authorization |
| Buffers | 2 in PSRAM, `CAMERA_GRAB_LATEST` |
| Wi-Fi | Station, power saving disabled |
| USB | USB Serial/JTAG console |

Pinout in `include/xiao_sense.hpp`: D0..D7 = 15,17,18,16,14,12,11,48; XCLK=10; PCLK=13; VSYNC=38; HREF=47; SDA=40; SCL=39; RESET/PWDN=-1. Image settings, mirroring, and the manual destination are in `include/settings.hpp`.

The fixed profile in `include/ov2640_profile.hpp` follows the sensor values from [OpenIris-ESPIDF CameraManager](https://github.com/EyeTrackVR/OpenIris-ESPIDF/blob/b64b0fa84ce0d1b073067f8758399d76033340ad/components/CameraManager/CameraManager/CameraManager.cpp): grayscale effect 2, saturation -2, brightness/contrast 2, manual exposure 300, and manual gain 2, plus correction and JPEG clock settings. Fixed exposure assumes suitable lighting; it has not been calibrated for your cameras. There is no color-video option.

For this transport, we use JPEG quality 10 (upstream uses 8), two PSRAM buffers, and `CAMERA_GRAB_LATEST`, prioritizing bandwidth and recent frames. The sensor still generates JPEG; neither the ESP32 nor the PC performs software conversion. Sending uses `sendmsg` with separate header and fragment buffers, eliminating one intermediate application copy; the network stack may still copy data. The protocol and 1,200-byte payload remain unchanged. Traffic reduction and actual FPS must be measured on hardware.

## Build

Open this directory in PlatformIO, or run from the repository root:

```powershell
pio run -d firmware -e xiao_esp32s3_ov2640
pio run -d firmware -e xiao_esp32s3_ov2640 -t upload --upload-port COM7
pio device monitor -p COM7 -b 115200
```

On Windows, if the project path contains spaces, use the wrapper, which prepares a build copy in a temporary directory without spaces:

```powershell
.\tools\build-firmware.ps1
.\tools\build-firmware.ps1 -Action upload -Port COM7
.\tools\build-firmware.ps1 -Action monitor -Port COM7
```

Replace COM7 with the actual port (`pio device list`). Flashing was not performed automatically. If necessary, hold BOOT while connecting USB, then release it to enter the bootloader.

The project automatically loads `platformio.local.ini` when present, including through PlatformIO in VS Code. The wrapper recognizes it too. On the development machine, it points to the already installed Xtensa 14.2.0+20260121 compiler. This file is ignored by Git because it contains a machine-specific path. Without it, the pinned defaults in `platformio.ini` apply.

The platform is pinned to `espressif32@6.13.0`, with explicit overrides for **ESP-IDF 5.5.5** (`framework-espidf@4.50505.0`) and **Xtensa GCC 14.2.0+20260121**. This combination uses the existing compiler version accepted by the SDK. The RISC-V compiler is pinned to `14.2.0+20241119` but does not participate in this firmware: the ULP coprocessor is disabled. Do not enable ULP without also aligning its toolchain with the SDK.

Managed driver: `espressif/esp32-camera@2.0.16`. The component is downloaded by the ESP-IDF Component Manager, not the Arduino framework. Keep the generated `dependencies.lock` to reproduce dependency resolution.

The project script `pio_compat.py` adapts SDK compilation response files to PlatformIO/SCons: it repairs quoting and expands arguments before classification. This preserves `-mlongcalls` and other required options, including for the bootloader. It does not modify the SDK or PlatformIO installation.

ESP-IDF/PlatformIO does not handle some arguments containing spaces in build paths correctly; CMake also expands 8.3 paths. The wrapper copies required files to `%TEMP%\iris-udp-<identifier>`, leaving the original sources in place and keeping the build cache there. After success, it copies `firmware.bin`, `firmware.elf`, `bootloader.bin`, and `partitions.bin` to `firmware/artifacts/` and updates `dependencies.lock`. If you use `secrets.h`, it is also present in the temporary copy. Alternatively, keep the entire project in a path without spaces, such as `C:\src\LocalImageProcessor`, with `common` alongside `firmware`.

## Single BIN for the Seeed web flasher

Run `.\tools\build-firmware.ps1 -Action build` from the repository root. Use `firmware/artifacts/firmware-merged.bin` in the [Seeed ESP Flasher](https://www.seeedstudio.com/xiao-series-page/playground/esp-flasher/): connect the XIAO ESP32S3, select **Add BIN files**, choose this file, and verify its flash address is **0x0** before flashing. No additional bootloader or partition file is needed. This merged image contains the build's bootloader, partition table, and application at their generated offsets. PlatformIO also generates it in `.pio/build/xiao_esp32s3_ov2640/`.

Use the merged image for a full installation. Its padding covers NVS, so flashing it resets saved Wi-Fi/pairing data; the configured `wifi.ini` values are applied on boot. For an application-only update preserving NVS, use `firmware.bin` at `0x10000` with the existing matching partition table, without erasing the flash.

## Configure Wi-Fi before building

Edit `firmware/wifi.ini` (or copy `wifi.example.ini` to that name):

```ini
[wifi]
ssid = MyNetwork
password = MyPassword
```

Use a 2.4 GHz network and unquoted values. Characters such as `%`, `#`, `;`, quotes, and slashes inside values are preserved; the INI reader trims leading and trailing whitespace. Build/flash normally using the wrapper. The build generates a private header without placing the password in compiler arguments. The INI and generated header are ignored by Git, but credentials remain in the binary and temporary build copy.

Every build generates a new identifier. On that firmware's first boot, the INI SSID and password replace the previous NVS values. Subsequent USB changes survive normal restarts. Building and flashing again reapplies the INI even if it was not edited. Reflashing the exact same binary that was already applied does not reapply these values. An empty SSID disables this override and retains USB configuration (or the legacy `secrets.h`, if present).

## Change Wi-Fi over USB, persisted in NVS

Firmware without credentials starts the console and waits for configuration. Send a JSON line followed by Enter:

```json
{"command":"wifi","ssid":"MyNetwork","password":"MyPassword"}
```

The firmware saves the settings to NVS and restarts. It does not print the credentials. Use a serial terminal with line sending and local echo disabled if you do not want the password displayed. The monitor and uploader cannot use the same port simultaneously.

Other commands:

```json
{"command":"status"}
{"command":"scan"}
{"command":"unpair"}
```

`status` reports ID, boot, Wi-Fi state, and free heap. `unpair` erases only the server ID and restarts; it keeps Wi-Fi settings. If `preferred_server` is fixed in code, that server remains selected.

An alternative for initial boot is to copy `include/secrets.example.h` to `include/secrets.h` and fill in the SSID/password. This file is ignored by Git. Credentials already saved in NVS take precedence over these legacy initial values. Do not publish binaries containing credentials. Passwords do not need to be placed in `platformio.ini`.

`status` also shows the configured SSID, without the password. `scan` temporarily interrupts the connection and lists up to 20 detected networks with channel, RSSI, and the numeric ESP-IDF authentication mode. It resumes connection attempts afterward. Run it after configuring Wi-Fi; it does not change credentials.

## Operation

- Stable ID derived from the six Wi-Fi MAC bytes. Random boot ID on each restart.
- UDP discovery/heartbeat every second, separate from the capture/sending task.
- The first valid ACK creates persistent pairing. Use `preferred_server` to prevent pairing with another PC during initial setup.
- Video is sent only while an ACK has been received within the last 4 seconds.
- Nonblocking video socket; failure on any fragment abandons the rest of that frame.
- Automatic Wi-Fi reconnection; discovery continues after obtaining a new IP.
- The console keeps configuration and diagnostics available, including before Wi-Fi setup.
- No HTTP stream on the ESP32. All HTTP MJPEG output is hosted on the PC.

The driver detects the sensor and logs its PID. Camera initialization failures are reported and retried every 5 seconds. Check PSRAM, power, sensor, and ribbon cable if initialization fails. UVC, OTA, mDNS, and the original OpenIris REST commands are not implemented.
