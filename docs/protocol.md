# OIRB v1 - binary format

All integers are unsigned, in **network byte order / big endian**. Raw C++ structs are not transmitted. Shared encoder/decoder: `common/protocol.hpp`.

| Offset | Bytes | Field |
|---:|---:|---|
| 0 | 4 | ASCII `OIRB` |
| 4 | 1 | Version = 1 |
| 5 | 1 | Type: HELLO=1, ACCEPT=2, JPEG=3 |
| 6 | 2 | Payload length |
| 8 | 8 | Camera ID: 48-bit MAC, upper two bytes zero |
| 16 | 4 | Random, nonzero boot ID |
| 20 | 4 | Frame ID |
| 24 | 8 | Capture timestamp in microseconds on the device clock |
| 32 | 2 | Zero-based fragment index |
| 34 | 2 | Fragment count |
| 36 | 4 | Total JPEG size |
| 40 | 8 | Persistent server ID |
| 48 | variable | Payload |

Exact UDP payload length = 48 + payload length. Up to 1,200 bytes of fragment payload. IPv4 + UDP + OIRB + payload totals 1,276 bytes for the largest packet, below the usual 1,500-byte MTU. Networks with smaller MTUs still require evaluation.

## Control, port 5001

**HELLO:** the camera sends from an ephemeral socket to broadcast `255.255.255.255:5001` or a manual IP. Camera/boot are required. Server ID=0 allows initial pairing; afterward it contains the persisted ID. Optional informational payload up to 128 bytes, for example `xiao-esp32s3-sense;OV2640;iris-udp/1.0;JPEG;240x240;monochrome`. Frame/index/count/total must be zero at the source; the receiver does not use these fields for control messages.

**ACCEPT:** unicast reply to the HELLO source. Repeats camera/boot and supplies a nonzero server ID. Four-byte payload: video UDP port uint16 + maximum FPS uint16 (1..90). The firmware uses the lower of its local limit and the offered limit. There is no explicit retransmission; HELLO repeats every second and renews presence. The PC tracks registration by ID, not IP.

Registration remains authorized for 6 seconds on the PC. A camera stops transmitting after 4 seconds without ACCEPT. A boot change clears reassembly and the previously published frame. A different IP for an active ID is rejected until the previous presence expires. This detects simple collisions but does not prove identity or prevent spoofing.

## Video, port 5000

Each fragment carries camera/boot/server values matching registration. `total` is between 4 and 262144; `count=ceil(total/1200)`; `index<count`. All fragments except the last contain 1,200 bytes. The last contains the exact remainder. Total size, count, and timestamp remain identical across all fragments of a frame.

Frame IDs use 32-bit modular ordering. The difference must be less than 2^31 to count as newer. A new boot allows IDs to restart. A newer frame abandons the previous one even if incomplete. Reassembly timeout starts at the first observed fragment and is not extended by duplicates. Late fragments cannot revive an expired frame.

A JPEG is published only after all unique fragments arrive and the initial `FFD8` and final `FFD9` markers are present. Validation does not guarantee that the entire JPEG decodes. This version has no additional CRC, FEC, or retransmission. The timestamp is carried but is not interpreted as absolute latency.

## Local HTTP

`GET /camera/N.mjpeg`, where N=1..3 is paired. Response: `200`, `Content-Type: multipart/x-mixed-replace; boundary=frame`, caching disabled, connection closed on shutdown. Each part:

```text
--frame\r\n
Content-Type: image/jpeg\r\n
Content-Length: <bytes>\r\n
\r\n
<JPEG>\r\n
```

There is no global Content-Length or HTTP chunked encoding. An interrupted connection does not require a final delimiter. A slow client has a 1-second send deadline per part; there is no per-client frame queue.
