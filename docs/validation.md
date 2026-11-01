# Validation and references

## Automatically validated scope

- C++17 build with MSVC/Windows x64.
- Complete firmware build, including bootloader, with ESP-IDF 5.5.5, PlatformIO `espressif32@6.13.0`, and Xtensa GCC `14.2.0+20260121`, reusing the compiler installed on the development machine.
- ESP32-S3 image checked with `esptool image_info`: valid checksum and hash. Initial `firmware.bin`: 861,344 bytes; linker-reported static RAM: 43,788 bytes (excludes runtime allocations, JPEG buffers, and tasks).
- Generated configuration checked: 8 MB DIO flash, octal PSRAM at 80 MHz, USB Serial/JTAG console, ULP disabled.
- Additional POSIX-path compilation with GCC/Cygwin; native Linux/macOS were not run.
- Unit tests for the wire format and reassembly.
- Actual executable with three simulated UDP producers and three HTTP readers.
- Reordered, duplicate, missing, late, and malformed fragments.
- Boot changes; rejection of packets from previous sessions, wrong servers, and unknown sources.
- Expiration without waiting for the next frame to arrive.
- A non-reading HTTP client does not block a healthy consumer and is disconnected.
- Two new HTTP subscribers reject an aged cached frame while the camera is still within its offline timeout, then both receive the next complete frame from one publication.
- Per-client HTTP metrics include periodic and final reports, expired-frame accounting, successful sends, and slow-client send failures.
- Offline/reconnection states and clean shutdown.
- Bridge restart and changed registration order preserve IDs/endpoints.
- An allowlist with a free slot rejects unknown IDs; active-ID collisions at another IP are rejected.
- Simulated migration between loopback addresses after lease expiration preserves the endpoint.

Tests use synthetic structural JPEGs to verify byte preservation, not real image quality. No physical device was flashed automatically during implementation. Firmware build results must be distinguished from validation on the actual sensor.

Both CTest tests (`protocol` and `integration`) passed. Firmware artifacts are in `firmware/artifacts/`; the local build log is `build/firmware-build.log`. The Windows wrapper uses a temporary directory without spaces. `firmware/pio_compat.py` adapts ESP-IDF compile response files to PlatformIO/SCons parsing, preserving options such as `-mlongcalls` without modifying installed packages.

## Hardware test procedure

Monochrome update: firmware built with the OpenIris OV2640 profile, 240x240 JPEG, and two-segment UDP sending through `sendmsg`. Image quality, exposure, datagram reception, and performance still require validation on physical boards. The 90 FPS ceiling is not a measurement. Binary size values above refer to the initial build.

1. Build/flash a XIAO with the OV2640 profile; verify sensor identification and PSRAM in the serial log.
2. Provision Wi-Fi, start the bridge, and confirm automatic registration without entering an IP.
3. Repeat for the other two boards; assign `left_eye`, `right_eye`, and `face` by ID.
4. Open the URLs in the HTTP MJPEG consumer; confirm orientation and resolution.
5. Power-cycle one camera: the others should continue, and its endpoint should stay unchanged.
6. Renew DHCP and verify recovery. If the IP changes during an active lease, allow up to 6 seconds.
7. Stop/restart the bridge while preserving `pairings.txt`; cameras should return automatically.
8. Enable `auto_pair=false` and verify that a fourth unknown source is rejected.
9. Block broadcasts: serial diagnostics should appear; configure a manual IP and test unicast.
10. Test tracking in the consumer. Compare with the original HTTP transport under equivalent conditions (1/2/3 cameras).
11. Run for hours, recording CPU/RAM/FPS/loss/age. Watch for growing delay.
12. Repeat with the PC on Ethernet, 20 MHz Wi-Fi, and adequate signal. Adjust quality/FPS/resolution only based on measurements.

The configured ceiling is 90 FPS per camera, not a measured result. Actual FPS depends on the sensor, resolution, exposure, and network. There is no automated test of radio interference, real DHCP, or tracking.

## Implementation decisions

- C++ on the PC as explicitly requested, replacing the suggested Python prototype.
- Standalone ESP-IDF firmware using `esp32-camera`, recreating the minimal transport path. The complete upstream firmware was not incorporated.
- The requested XIAO Sense/OV2640 profile instead of a generic ESP32S3.
- Strict INI validation without a YAML dependency.
- UDP broadcast discovery plus manual unicast, an alternative allowed by the specification. mDNS/DNS-SD is not implemented.
- Local pairing using MAC/server ID and an allowlist; no encryption.
- USB controls for Wi-Fi, diagnostics, and clearing pairing; no dependency on the OpenIris API in the consumer.
- Baballonia integration limited to generic HTTP MJPEG output, as clarified by the user.

## Consulted sources

Consulted on October 31, 2026:

- [OpenIris-ESPIDF](https://github.com/EyeTrackVR/OpenIris-ESPIDF), commit `b64b0fa84ce0d1b073067f8758399d76033340ad`: README, camera component, and `boards/seed_studio/xiao_esp32s3`. Used to check architecture and hardware profile; not a build dependency.
- [Seeed: XIAO ESP32S3 Sense camera](https://wiki.seeedstudio.com/xiao_esp32s3_camera_usage/): connector pinout and capture/PSRAM configuration.
- [Espressif esp32-camera](https://github.com/espressif/esp32-camera): capture driver and JPEG API.
- [PlatformIO ESP-IDF](https://docs.platformio.org/en/latest/frameworks/espidf.html): project structure and managed components.
- [ESP-IDF 5.5.5 tools](https://github.com/espressif/esp-idf/blob/v5.5.5/tools/tools.json): confirms support for the existing Xtensa `esp-14.2.0_20260121` compiler. ESP-IDF 5.4.0 rejected that version during compatibility checks; the project was aligned with SDK 5.5.5 without disabling those checks.
- [Baballonia](https://github.com/Project-Babble/Baballonia), commit `3f85119d786b57bd9f461177f71415c682aa9f4b`: `src/Baballonia.IPCameraCapture/IpCameraCaptureFactory.cs` accepts `http://`; `IPCameraCapture.cs` reads the stream and identifies SOI/EOI. Input compatibility was inspected in code; a tracking test was not performed.

Reference copies in `.reference/` are ignored by Git and are not part of the distributed application. External components retain their respective project licenses.
