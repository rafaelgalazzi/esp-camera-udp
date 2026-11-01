# OpenIris UDP Bridge - Project specification

> This is the translated original proposal. It describes planned requirements and alternatives, not necessarily the current implementation. See [README.md](README.md) for current settings and usage.

## 1. Overview

Develop a low-latency solution for **three ESP32 cameras with OpenIris-based firmware**, connected over 2.4 GHz Wi-Fi. Each camera captures eye images and transmits JPEG frames over **UDP** to a local PC application. The application is **headless (no graphical interface)**, runs in the terminal, reconstructs frames, and provides **three local HTTP MJPEG streams**, initially for **Baballonia**, which remains responsible for image processing and eye tracking.

**Intended flow:**

```text
ESP32 1 -- UDP/JPEG --\
ESP32 2 -- UDP/JPEG ----> PC: OpenIris UDP Bridge ---> HTTP MJPEG localhost ---> Baballonia ---> tracking
ESP32 3 -- UDP/JPEG --/
```

**Goal:** remove HTTP MJPEG transmission from the Wi-Fi segment, a potential source of blocking and queues, while maintaining compatibility with the receiving application's familiar video format without reimplementing tracking.

> **Important limitation:** UDP does not make the network immune to interference or guarantee uninterrupted streaming. The goal is to avoid *prolonged freezes and accumulated delay*: incomplete packets or frames should be discarded, and the system should always prioritize the latest complete frame.

## 2. Current situation and motivation

- Three ESP32 cameras share a 2.4 GHz Wi-Fi network.
- The original OpenIris streams MJPEG over HTTP; simultaneous streams may experience quality drops, freezes, and latency.
- The current transport uses HTTP over TCP, which preserves byte ordering and may accumulate delay during loss/retransmission.
- The proposal is **not** simply to replace TCP with UDP: it requires a frame protocol with explicit buffer limits, frame identification, and late-data dropping.
- The original OpenIris project is archived and points to newer development; evaluate the selected firmware base before integrating changes.

## 3. Scope

### Included in the MVP

1. Recreate/adapt ESP32 firmware as a **buildable PlatformIO project**, with JPEG capture, UDP sending, PC discovery, and appropriate hardware profiles.
2. Develop a local application that **automatically discovers cameras on the same network** and receives all three streams simultaneously without manually entering IP addresses.
3. Reconstruct each camera's images, validate packets, and discard incomplete or outdated frames.
4. Provide a separate HTTP MJPEG endpoint per camera, bound to `127.0.0.1` by default.
5. Show per-camera terminal metrics without a GUI: received FPS, complete FPS, losses, dropped frames, bytes/s, latest-frame age, and connection state.
6. Test local URL integration with Baballonia and adjust the HTTP MJPEG format as necessary.
7. Implement automatic recovery from Wi-Fi drops, restarts, and stream interruptions.
8. Provide persistent camera pairing and identification, assigning each device a configurable role: left eye, right eye, or face.

### Outside the initial scope

- Creating a new eye-tracking algorithm in the intermediary application.
- Creating a GUI, web dashboard, or video preview window in the MVP.
- Guaranteeing fixed FPS or zero frame loss in every Wi-Fi environment.
- Modifying Baballonia internals without a demonstrated need.
- Using reliable retransmission of old frames as the default behavior.

## 4. Architecture

### 4.1 ESP32 firmware (producer)

- Capture images using the available camera settings.
- Compress JPEG at the source when supported by the sensor/firmware; for grayscale sources, confirm JPEG encoding support or cost.
- Split JPEGs into safely sized datagrams, initially suggesting **about 1,200 payload bytes** per packet to reduce IP fragmentation risk.
- Add a binary header with protocol version, camera ID, frame ID, packet index, packet count, data length, and capture timestamp.
- Discover the PC automatically using mDNS/DNS-SD (preferred), or respond to UDP announcements/probes on the LAN; allow a manually configured destination as fallback.
- Send UDP packets to the discovered PC IP/port; **one receiving port, such as `5000`, can serve all three cameras** because the header identifies the source. Separate ports are optional.
- Limit frame rate and JPEG quality through configuration.
- Prioritize current capture and avoid unbounded queues.
- Keep a configuration/diagnostic channel, such as control HTTP, if compatible with the chosen firmware; retaining MJPEG over Wi-Fi is unnecessary.

### 4.2 Local PC application (bridge)

**Automatic discovery:** the PC advertises an mDNS/DNS-SD service discovered by the ESP32s, and the PC discovers the cameras, reusing existing OpenIris mDNS capabilities after validating service names/types. Fallback options are subnet UDP broadcast beacons/probes or a manual IP. Discovery must find new cameras, detect restarts, handle IP changes, and report multicast/broadcast blocking.

**UDP input layer:** sockets receive packets, identify cameras by stable identifiers, and separate stream state.

**Reassembly:** a bounded temporary buffer per camera; out-of-order reassembly; duplicate, missing-packet, and frame-expiration detection; immediate incomplete-JPEG discard after timeout.

**Frame layer:** a queue containing **one latest complete frame** per camera (*latest-frame-wins*). Slow clients must not cause image queues to grow.

**HTTP output server:** provide `multipart/x-mixed-replace` with `image/jpeg` so each input can be consumed as a local MJPEG stream. Each connection keeps a short output queue and closes/reconnects stalled clients.

**Interface/monitoring:** **terminal only (simple text CLI/TUI), without windows, a web dashboard, or video preview**. Show detected camera, persistent ID, current IP, assigned role, state, and per-camera metrics; log discovery, connection, disconnection, and reconnection events. Initially define assignments and parameters through `config.yaml` or CLI arguments, without graphical menus. Assignments survive IP changes and restarts. Logs must be readable, with optional JSON output for future automation.

### 4.3 Baballonia integration (consumer)

**Proposed** endpoints, not already verified APIs:

```text
http://127.0.0.1:8080/camera/1.mjpeg
http://127.0.0.1:8080/camera/2.mjpeg
http://127.0.0.1:8080/camera/3.mjpeg
```

The local application hosts the streams, and **Baballonia performs tracking**. The bridge does not need to interpret eyes.

**Required validation:** confirm that the installed Baballonia version accepts local HTTP MJPEG URLs for each camera, and that multipart format, headers, resolution, orientation, and device identification are compatible. If Baballonia requires specific discovery, the OpenIris API, or another input mechanism, implement a *compatibility adapter* rather than assuming three URLs are sufficient.

HTTP MJPEG between processes on the same PC uses loopback, so Wi-Fi transport costs are not repeated at this stage. However, the client can still be slow: queues and timeouts remain necessary.

## 4.4 Headless operation and terminal observability

**Product requirement:** the program starts from the terminal, automatically discovers cameras without preconfigured IPs, publishes local endpoints, and prints states and metrics. **It must not open any windows.** MJPEG is served quietly in the background while the process runs.

Illustrative log with fictional addresses and values:

```text
$ openiris-udp-bridge --config config.yaml
[12:00:00] INFO  UDP listening on 0.0.0.0:5000 | HTTP on 127.0.0.1:8080
[12:00:01] FOUND camera=cam-a31f ip=192.168.1.101 role=left_eye
[12:00:01] FOUND camera=cam-b42e ip=192.168.1.102 role=right_eye
[12:00:02] FOUND camera=cam-c53d ip=192.168.1.103 role=face
[12:00:05] STATS cam-a31f ONLINE fps=29.8 loss=1.2% bitrate=2.4Mbps last=18ms
[12:00:05] STATS cam-b42e ONLINE fps=29.4 loss=2.0% bitrate=2.3Mbps last=21ms
[12:00:05] STATS cam-c53d ONLINE fps=30.0 loss=0.4% bitrate=2.5Mbps last=15ms
[12:00:06] HTTP  /camera/1.mjpeg  /camera/2.mjpeg  /camera/3.mjpeg
[12:00:12] WARN  cam-b42e OFFLINE (no complete frame for 3s)
[12:00:14] INFO  cam-b42e RECONNECTED ip=192.168.1.102
```

- Log events immediately; print a statistics summary every **5 seconds** by default, configurable to avoid terminal clutter.
- Per-camera states: `DISCOVERED`, `CONNECTING`, `STREAMING`, `DEGRADED`, `OFFLINE`.
- Metrics: complete FPS, estimated lost packets, incomplete/dropped frames, receive rate, time since the last complete frame, source IP, and reconnection count.
- Do not report absolute capture latency without ESP32/PC clock synchronization; show local frame age and, if clocks are synchronized, estimated end-to-end latency.
- Support `--log-level`, `--stats-interval`, and `--config`; shut down cleanly with `Ctrl+C`.
- Without initial role configuration, list cameras by ID and allow later configuration-file edits. Endpoints must remain stable across IP changes.

Example initial configuration with illustrative values:

```yaml
udp:
  listen_host: 0.0.0.0
  port: 5000
http:
  host: 127.0.0.1
  port: 8080
discovery:
  mdns: true
  udp_broadcast_fallback: true
monitoring:
  stats_interval_seconds: 5
  log_level: INFO
cameras:
  cam-a31f: {role: left_eye, endpoint: 1}
  cam-b42e: {role: right_eye, endpoint: 2}
  cam-c53d: {role: face, endpoint: 3}
```

Example IDs are not real IDs. Automatic discovery depends on corresponding discovery/registration support in ESP32 firmware and a router that permits the necessary local communication.

## 5. Automatic discovery, pairing, and reconnection

**Mandatory requirement:** when the PC application opens, ESP32s on the same LAN must be found automatically without entering their IPs. The recreated firmware must implement its side of this protocol.

### 5.1 Recommended discovery

1. The PC advertises a receiving service, for example `_openiris-udp._udp.local`, through **mDNS/DNS-SD**, including port and protocol version. The name/type are **new proposals for this bridge**, not official OpenIris interfaces.
2. Each ESP32 locates the service, obtains the PC address/port, and registers through a control message containing stable camera ID, name, model, firmware version, capabilities, and current address.
3. The PC maintains an inventory of seen devices, including temporarily offline cameras; persistently associates each ID with **left eye**, **right eye**, or **face**; and logs that assignment in the terminal.
4. The PC confirms registration and authorizes streaming, supplying parameters such as maximum FPS and UDP port.
5. Heartbeats and presence timeouts indicate disconnection; after an ESP32 restarts or changes IP, the application recognizes its **persistent ID**, never just the IP.
6. If mDNS does not work, provide **UDP broadcast** subnet discovery and manual addressing. Broadcast generally does not cross subnets/VLANs and may be blocked by client isolation; document this.

The original OpenIris already mentions **mDNS** for discovery and tracker communication; reuse that idea where possible. Do not assume existing firmware advertises this new service: it must be added or adapted. To avoid ambiguity with multiple computers, support selection/pairing with the correct PC and persist the server identifier.

### 5.2 Reliability rules

- Separate **control/discovery** traffic from continuous frame transmission; discovery must not block video reception.
- Do not rely on broadcasts alone to assess stream health: monitor the last complete JPEG received.
- Generate or derive unique stable identifiers and validate collisions to prevent identical IDs across devices.
- Limit and validate announcements/frames from unknown devices; offer **pairing/allowlists** to prevent unauthorized local sources.
- Do not assume multicast/mDNS works with every router configuration; provide fallback and specific diagnostic messages.
- Persist stable-ID-to-role assignments; restore the corresponding local endpoint after reconnection, even if the IP changes.

## 6. Proposed UDP protocol (v1)

Each datagram contains `header + payload`. Finalize the detailed specification before implementation:

| Field | Purpose |
|---|---|
| Magic + version | Identify packet and version |
| Camera ID | Identify stream |
| Frame ID (uint32) | Detect new/old frames |
| Packet index (uint16) | Reorder segments |
| Packet count (uint16) | Determine when the JPEG is complete |
| Capture timestamp | Measure delay since capture |
| Payload length | Validate received size |
| Payload | JPEG bytes |

Rules:

- Keep datagrams below the effective MTU to avoid IP fragmentation.
- Bound the number of frames in reassembly and memory usage per camera.
- Validate indices, sizes, and identifiers before allocating memory.
- Accept packet reordering; discard duplicates.
- Abandon incomplete frames after a configurable timeout.
- If a newer frame is available, never block the stream waiting for an older one.
- Consider a frame CRC/checksum if tests justify its cost.
- Version the protocol to support future evolution.

**Note:** a single lost datagram can invalidate the entire JPEG, so practical testing must guide packet size, FPS, compression, and timeout adjustments.

## 7. Suggested technologies

- **Firmware:** a C/C++ **PlatformIO** project with `platformio.ini`, preferably ESP-IDF (`framework = espidf`), reusing compatible OpenIris code. Exact board, sensor, pinout, and PSRAM selection is mandatory.
- **PC prototype:** Python with `asyncio` or sockets, an asynchronous HTTP server (`aiohttp` or equivalent), and terminal-only metrics (`logging`; optional `rich`, without GUI dependencies).
- **Production (optional):** C++/Rust/.NET if measurements reveal prototype bottlenecks. Do not migrate without a demonstrated need.
- **OpenCV:** optional debugging/visualization only; there is no need to decode and re-encode JPEG on every frame when the bridge only forwards bytes.

## 7.1 Firmware delivery requirement: PlatformIO

ESP32 firmware must be **delivered as a PlatformIO project**, with `platformio.ini` at its root, C/C++ source, declared dependencies, configuration files, and build/flash/serial-monitor documentation. Users should be able to open it in VS Code with the PlatformIO extension, compile, and flash each ESP32 without relying on precompiled binaries.

**Proposed** structure, not necessarily the current OpenIris repository layout:

```text
firmware/
|-- platformio.ini
|-- CMakeLists.txt               # when using ESP-IDF
|-- sdkconfig.defaults           # if needed; no secrets
|-- boards/                      # pinout/sensor/PSRAM profiles
|-- include/
|-- src/
|   |-- CMakeLists.txt           # when using ESP-IDF
|   |-- main.cpp
|   |-- camera_capture.cpp
|   |-- udp_streamer.cpp
|   |-- discovery.cpp
|   `-- wifi_manager.cpp
`-- README.md
```

**Illustrative** configuration; **do not use `esp32-s3-devkitc-1` without confirming the actual board and sensor**:

```ini
[env:esp32s3_example]
platform = espressif32
board = esp32-s3-devkitc-1
framework = espidf
monitor_speed = 115200
```

Expected commands after selecting the correct board and port:

```bash
pio run -e esp32s3_example
pio run -e esp32s3_example -t upload
pio device monitor -b 115200
```

Additional requirements:

- Create **one environment per hardware variant** actually used; do not assume all three modules share a pinout.
- Document sensor, pinout, clock, flash, PSRAM, JPEG quality, FPS, Wi-Fi credentials (outside version control), and UDP protocol parameters.
- Preserve persistent Wi-Fi settings, stable ID, serial diagnostics, and network-loss recovery where possible.
- Provide flashing, initial setup, and automatic discovery testing instructions.
- Avoid including Wi-Fi passwords in `platformio.ini` or version control.
- When porting **OpenIris-ESPIDF** code to PlatformIO, validate CMake, components, `sdkconfig`, ESP-IDF versions, and `boards/` profiles; **do not assume direct compatibility without adaptations**, since upstream documents a native ESP-IDF workflow.
- If the chosen board is unsupported by the selected PlatformIO/ESP-IDF versions, record the blocker and adjust the environment before proceeding.

Toolchain references:

- PlatformIO + ESP-IDF: https://docs.platformio.org/en/latest/frameworks/espidf.html
- `platformio.ini` configuration: https://docs.platformio.org/en/latest/projectconf/index.html
- New firmware source repository: https://github.com/EyeTrackVR/OpenIris-ESPIDF

## 8. Performance goals and acceptance criteria

Initial goals, subject to measurement on real hardware:

1. Receive **3 simultaneous streams** without cross-camera blocking.
2. Operate at FPS supported by devices and network, targeting **stable 30 FPS per camera** as the first milestone, followed by 60 FPS tests.
3. Avoid increasing latency over time: do not accumulate old-frame queues.
4. Automatically recover streaming after packet loss or an ESP32 restart.
5. Keep each local endpoint independently accessible.
6. Measure and log FPS, loss rate, dropped frames, estimated delay, and CPU/RAM usage.
7. Confirm working Baballonia tracking with **three inputs**, if supported/configurable in the selected version.
8. Automatically detect all three LAN cameras, remember their roles, and rediscover them after restarts or IP changes.

**Do not consider the project complete just because images appear in a browser:** final validation requires practical Baballonia testing, measuring perceived latency and stability during extended sessions.

## 9. Planned tests

| Test | Procedure | What to verify |
|---|---|---|
| Baseline | 1, 2, and 3 cameras with current HTTP MJPEG | FPS, freezes, traffic |
| Isolated UDP | One ESP32 sending to the PC | Reassembly and loss |
| Concurrency | Three simultaneous ESP32s | No cross-camera blocking |
| Loss | Simulate datagram dropping/reordering | Timeout and frame discard |
| Reconnection | Power-cycle a camera | Automatic recovery |
| Automatic discovery | Start the app with three LAN cameras without entering IPs | Find all three; list identity/IP/state |
| IP change | Renew DHCP/restart router | Recognize camera ID and restore endpoint without intervention |
| Blocked mDNS | Block multicast or enable client isolation | Diagnostics and alternative discovery/manual fallback |
| Slow client | MJPEG consumer reading slowly | Bounded queue; recent frames |
| Integration | Baballonia consuming local streams | Working tracking and compatibility |
| Long duration | Run for hours | Stable memory; no progressively increasing latency |

Also test with the PC connected by **Ethernet**, a less congested 20 MHz Wi-Fi channel, and adequate signal for all cameras. UDP does not remove radio limitations.

## 10. Implementation plan

### Phase 1 - Protocol and proof of concept

- Select exact ESP32 model, sensor, and firmware base; define PlatformIO environment and board/PSRAM configuration.
- Document the binary datagram format.
- Implement UDP sending on one camera.
- Create a receiver that reassembles JPEG and saves/displays frames.

### Phase 2 - Multiplexing and robustness

- Support three cameras and implement automatic discovery (mDNS/DNS-SD, UDP broadcast fallback), registration, and heartbeats.
- Create persistent identification and assignments through a configuration file and terminal messages for left eye, right eye, and face.
- Add timeouts, frame dropping, memory limits, and metrics.
- Test disconnections and interference.

### Phase 3 - Local HTTP streaming

- Implement loopback MJPEG endpoints.
- Prevent slow clients from blocking UDP reception.
- Validate using a browser or independent MJPEG client.

### Phase 4 - Baballonia

- Confirm the supported video input mechanism.
- Integrate each stream with its corresponding camera.
- Adjust headers, discovery, or controls as needed.
- Test eye/face tracking according to actual Baballonia support.

### Phase 5 - Optimization

- Measure latency and bottlenecks before adjusting FPS, compression, datagrams, and buffers.
- Add persistent configuration, terminal/file logging, automatic process/service startup, and application distribution without a GUI.

## 11. Risks and mitigations

| Risk | Mitigation |
|---|---|
| 2.4 GHz Wi-Fi interference | Dedicated AP, good signal, suitable channels, bitrate control |
| UDP loss invalidates JPEGs | Small frames, lower FPS/bitrate, fast dropping, metrics |
| Baballonia rejects simple endpoints | Adapter for the supported input after code/API validation |
| Slow MJPEG client creates queues | One-frame buffer; timeout, dropping, and slow-client disconnection |
| Firmware lacks expected camera/JPEG support | Validate board, sensor, and pipeline before porting |
| High PC CPU usage | Forward JPEG without transcoding; profile before optimizing |

## 12. Open questions

- What are the exact models of the three ESP32 boards and their sensors?
- Which OpenIris version/branch will be used?
- Which Baballonia version will be used, and how are its video sources configured?
- Do the three inputs correspond to left eye, right eye, and facial camera?
- What FPS, resolution, and average JPEG size are currently measured?
- Is the PC connected by Ethernet or Wi-Fi?

## 13. References and basis for recreating ESP32 firmware

**Source code and implementation documentation:**

- **Original OpenIris firmware (archived source):** https://github.com/EyeTrackVR/OpenIris
- **Official EyeTrackVR documentation, including firmware setup and information:** https://docs.eyetrackvr.dev/
- **New ESP-IDF firmware rewritten in C/C++, recommended for evaluating UDP changes:** https://github.com/EyeTrackVR/OpenIris-ESPIDF
- **Original OpenIris releases/prebuilt firmware:** https://github.com/EyeTrackVR/OpenIris/releases
- **OpenIris-ESPIDF releases:** https://github.com/EyeTrackVR/OpenIris-ESPIDF/releases
- **Baballonia consumer source code:** https://github.com/Project-Babble/Baballonia
- **Baballonia repository documentation:** https://github.com/Project-Babble/BabbleDocs/blob/main/docs/software/baballonia/baballonia.mdx

**Implementation guidance:** clone the firmware repository appropriate for the board, identify capture, Wi-Fi, mDNS discovery, and streaming modules, then **replace/add UDP output while retaining necessary controls**. For OpenIris-ESPIDF, start with the README, source directories, and board profiles; select the exact profile before building and flashing. Do not use a binary for a different board.

Original OpenIris claims mDNS support and lists socket streaming in its TODO; this **does not prove** the new UDP output already exists. The automatic discovery and transport protocol specified here require new implementations or adaptations that must be tested. Baballonia's exact input interface also requires checking against the code/version in use.

---

**Summary:** a **terminal-only application, no GUI**; recreate/adapt ESP32 firmware **as a PlatformIO project** for JPEG capture, **automatic discovery of the PC bridge**, and UDP; automatically discover/pair three cameras and publish **UDP to local MJPEG**, prioritizing recent frames, automatic recovery, and validated Baballonia compatibility. The expectation is to **reduce freezes and accumulated latency**, not promise zero loss or freezing.
