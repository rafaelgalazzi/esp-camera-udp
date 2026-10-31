#pragma once
// Configuration without credentials. Camera expansion: XIAO ESP32S3 Sense / OV2640.
constexpr int camera_xclk_hz = 20000000;
constexpr int camera_jpeg_quality = 10; // 0..63, lower = better quality / more bandwidth
constexpr unsigned camera_fps = 90;
constexpr bool camera_mirror = false, camera_flip = false;
constexpr unsigned discovery_port = 5001;
// Empty uses automatic broadcast discovery. IPv4 address only, no DNS name.
constexpr const char* manual_bridge_ip = "";
// Set a known bridge server ID here to select a PC before first pairing.
constexpr unsigned long long preferred_server = 0;
