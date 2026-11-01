# OpenIris UDP Bridge

Stream up to **three ESP32 cameras over UDP**, with a PC bridge that exposes each video feed as **local HTTP MJPEG** for a browser or tracking application.

The firmware uses **ESP-IDF/C++** and the desktop application uses **C++17**, operated from the terminal.

```text
OV2640 -> JPEG on ESP32 -> UDP/Wi-Fi -> C++ bridge -> local HTTP MJPEG -> consumer
```

The bridge reassembles frames and forwards JPEG data without recompression. Automatic discovery, persistent pairing, and incomplete-frame dropping keep each camera independent.

| Current configuration | Value |
|---|---|
| Hardware | Seeed Studio XIAO ESP32S3 Sense + OV2640 |
| Image | Black and white, 240 x 240 |
| JPEG | Quality 10; lower numbers mean higher quality |
| Sending limit | 90 FPS per camera; actual rate depends on hardware and network |
| Camera network | 2.4 GHz Wi-Fi, UDP |
| PC output | Three HTTP MJPEG endpoints at `127.0.0.1:8080` |

**Guide:** [Quick start](#quick-start) | [Build the PC application](#build-and-run-on-windows) | [Pairing](#discovery-and-pairing) | [Video](#view-the-streams) | [Troubleshooting](#quick-troubleshooting) | [Validation](#validation)

## Quick start

Run commands from the repository root. The PC application requires CMake >= 3.20 and Visual Studio 2022 with C++ build tools. Use PlatformIO for the boards. Python 3 is required for the integration test.

### 1. Configure Wi-Fi

Copy the template if `firmware/wifi.ini` does not exist yet:

```powershell
Copy-Item firmware/wifi.example.ini firmware/wifi.ini
```

Edit `firmware/wifi.ini`, without quotes around values:

```ini
[wifi]
ssid = NETWORK_NAME
password = NETWORK_PASSWORD
```

Each new build applies the INI settings on its first boot, replacing the previous configuration. Subsequent USB changes persist until the next build. The INI is ignored by Git; credentials are embedded in the generated binary.

### 2. Flash the camera

Connect the board over USB, identify its port, and replace `COM8` with that port:

```powershell
pio device list
.\tools\build-firmware.ps1 -Action upload -Port COM8
```

Close any serial monitor before flashing. The wrapper also supports building from directories containing spaces. Repeat for the other boards; each camera is identified by its factory MAC address.

### 3. Start the PC bridge

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
.\build\Release\openiris-udp-bridge.exe --config config.ini
```

Connect the PC to the same LAN as the cameras and allow inbound UDP on ports **5000 and 5001** through the private-network firewall. Once the terminal shows `STREAMING`, open one of the [video URLs](#view-the-streams) on the same computer.

## Build and run on Windows

Requires CMake >= 3.20 and Visual Studio 2022 with C++ build tools. The application has no external library dependencies.

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
.\build\Release\openiris-udp-bridge.exe --config config.ini
```

The executable can run outside the project directory: pass the configuration file path. `state_file` is relative to the working directory, so use an absolute path when running through a scheduler. The target PC must have the MSVC runtime installed.

Linux/macOS (the POSIX code path is implemented; this delivery was validated on Windows):

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/openiris-udp-bridge --config config.ini
```

Options:

```text
--config file.ini
--stats-interval 5
--log-level INFO|WARN|ERROR
--json
--help
```

Use `--json > bridge.log` to save one JSON event per line. `Ctrl+C` shuts down the receiver and HTTP clients. The application does not open windows.

## Firmware

See [firmware/README.md](firmware/README.md) for USB configuration, building, and flashing. The environment for the supported hardware is `xiao_esp32s3_ov2640`. The same binary can be flashed to all three boards; their factory MAC addresses identify them.

## Discovery and pairing

1. Start the bridge. It listens on UDP `5000` for video and `5001` for control.
2. Configure camera Wi-Fi through the INI before building or over USB, and connect the cameras to the same LAN.
3. Cameras send UDP broadcast probes every second. The bridge registers up to three IDs and replies by unicast with the video port, FPS limit, and persistent server ID.
4. Cameras save that ID in NVS. The bridge saves `pairings.txt`, preserving endpoints across restarts and IP changes.
5. Assign roles using the IDs shown in the terminal, then restart the bridge:

```ini
[camera.000012345678abcd]
endpoint=1
role=left_eye

[camera.000012345678abce]
endpoint=2
role=right_eye

[camera.000012345678abcf]
endpoint=3
role=face
```

These are example IDs. Initial automatic assignment follows discovery order and uses `unassigned`; it does not guess which camera views each eye. Do not remove `pairings.txt` for a routine update: it also contains the PC's identity. If you change endpoints, keep all three unique in the configuration.

After registering the cameras, `auto_pair=false` limits registration to known IDs or IDs declared in the INI. To select a specific PC before first pairing, set `preferred_server` in the firmware to the ID printed by the bridge. The USB `unpair` command clears camera pairing; an already paired camera ignores other PCs.

**Implemented discovery:** UDP broadcast, with an optional manual IPv4 destination in the firmware. The MVP uses the UDP alternative described in the project; it **does not advertise or query mDNS/DNS-SD**. Wi-Fi client isolation, VLANs, and firewalls may block discovery. A manual destination bypasses blocked broadcasts but still requires IP connectivity. On Windows, allow inbound UDP on ports 5000/5001 on the private network. HTTP binds to loopback by default.

The allowlist and server ID are pairing filters, **not cryptographic authentication**. IDs and video travel in clear text. Use a trusted LAN. To avoid collisions during migration, an active ID at another IP is accepted only after 6 seconds without a heartbeat from the previous IP.

## View the streams

| Camera | URL on the same PC |
|---|---|
| 1 | <http://127.0.0.1:8080/camera/1.mjpeg> |
| 2 | <http://127.0.0.1:8080/camera/2.mjpeg> |
| 3 | <http://127.0.0.1:8080/camera/3.mjpeg> |

Endpoints without a paired camera return 404. A paired but offline camera keeps its endpoint; the connection waits for new frames and resumes when the camera returns. The client may reconnect according to its own timeout.

The inspected Baballonia code includes `IpCameraCaptureFactory`, which accepts `http://` addresses, and `IpCameraCapture`, which consumes JPEGs from the HTTP stream. Select Wireless/IP Camera input and use the URL for the corresponding role, as supported by the installed interface. The bridge is independent of the consumer version. Code inspection confirms that the input path exists; **it is not a tracking test against the installed Baballonia application**. References and the inspected commit are in [docs/validation.md](docs/validation.md).

## Quick troubleshooting

```powershell
.\tools\build-firmware.ps1 -Action monitor -Port COM8
```

The monitor does not echo what you type. Send one line at a time, followed by Enter:

```json
{"command":"status"}
{"command":"scan"}
```

`status` shows the configured network and connection state. `scan` lists detected networks and temporarily interrupts streaming.

| Symptom | Check |
|---|---|
| `reason=201` | Check the SSID reported by `status`, the 2.4 GHz network, and the antenna; run `scan`. |
| Wi-Fi connected, but no camera on the PC | Check UDP firewall ports 5000/5001, the shared LAN, and router client isolation. |
| HTTP 404 | No camera is paired with that endpoint yet. |
| Serial monitor is silent | Send `status`; check the port and whether another application is using it. |
| Dark or blurry image | Check lighting and lens focus; the profile uses manual exposure. |

More details: [firmware and USB commands](firmware/README.md).

## Limits and behavior

<details>
<summary>Buffers, metrics, and packet validation</summary>

- 3 cameras, 8 simultaneous HTTP clients, maximum JPEG size of 256 KiB.
- Datagrams up to 1,248 bytes: 48 header bytes + 1,200 payload bytes.
- One frame being reassembled per camera. A newer ID discards the previous reassembly; fragments of the new frame may arrive out of order.
- Default reassembly timeout: 100 ms. Frames are not retransmitted.
- One latest complete frame per camera; each client holds at most one additional reference during sending. Slow clients skip frames and are disconnected when sending one frame exceeds 1 second.
- Packets from unregistered sources, old boots, wrong servers, invalid sizes, or JPEGs missing SOI/EOI are discarded. The complete JPEG bitstream is not decoded for validation.
- States: `DISCOVERED`, `CONNECTING`, `STREAMING`, `DEGRADED` (500 ms without a frame), `OFFLINE` (3 seconds by default). Heartbeats alone do not keep video health marked as good.
- Statistics: observed frames/s, complete frames/s, bytes/s, estimated loss, drops, duplicates, late packets, local frame age, reconnections, process CPU and RAM. CPU is a percentage of one core; POSIX reports peak RSS, while Windows reports the current working set.
- Estimated loss counts missing fragments from observed, abandoned frames; it cannot measure frames whose packets were all lost. Packets rejected before identifying a session do not enter camera counters.
- `age_ms` uses only the PC clock. It is not capture latency; clocks are not synchronized.

</details>

The configuration uses **strict INI** to keep the C++ binary free of YAML dependencies. Unknown options and duplicate endpoints are errors. Configuration and roles are read at startup; restart after editing.

## Validation

C++ tests cover the wire format, limits, reordering, duplicates, expiration, old IDs, wraparound, and invalid JPEGs. The integration test starts the actual executable, simulates three devices, reads HTTP multipart streams, and checks slow-client isolation, camera/bridge restarts, and persistence.

The application and firmware are C++; the integration test uses Python 3 without external dependencies. CMake registers this test when Python is available. It can also run directly:

```powershell
python tests/integration.py build/Release/openiris-udp-bridge.exe
```

Synthetic fixtures verify byte-for-byte transport, not image decoding. Tests with three physical cameras, interference, tracking, and hours-long sessions must run on the actual equipment. A 90 FPS ceiling does not guarantee that effective rate. See [docs/validation.md](docs/validation.md).

Protocol architecture: [docs/protocol.md](docs/protocol.md).

## References

The sensor profile is based on [OpenIris-ESPIDF](https://github.com/EyeTrackVR/OpenIris-ESPIDF). This project implements its own UDP transport; it is not a complete fork and does not include OpenIris UVC, OTA, LED controls, or REST APIs. The consumer handles tracking.

- [Firmware configuration and hardware](firmware/README.md)
- [UDP protocol](docs/protocol.md)
- [Validation and consulted sources](docs/validation.md)
- [Original project specification](PROJETO_OPENIRIS_UDP_BRIDGE.md)
