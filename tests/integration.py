"""Black-box test of the actual C++ executable. Python stdlib only; not part of the app."""
import argparse
import concurrent.futures
import os
from pathlib import Path
import signal
import socket
import struct
import subprocess
import tempfile
import time

HEADER = struct.Struct("!4sBBHQIIQHHIQ")


def packet(kind, camera, boot=1, frame=0, payload=b"", index=0, count=0, total=0, server=0):
    return HEADER.pack(b"OIRB", 1, kind, len(payload), camera, boot, frame, 12345, index, count, total, server) + payload


def free_port(tcp=False):
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM if tcp else socket.SOCK_DGRAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Stream:
    def __init__(self, port, endpoint, slow=False):
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.socket.settimeout(3)
        if slow:
            self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
        self.socket.connect(("127.0.0.1", port))
        self.socket.sendall(f"GET /camera/{endpoint}.mjpeg HTTP/1.1\r\nHost: localhost\r\n\r\n".encode())
        self.file = self.socket.makefile("rb")
        assert self.file.readline().startswith(b"HTTP/1.1 200")
        self.headers()

    def headers(self):
        headers = {}
        while True:
            line = self.file.readline()
            if line == b"\r\n":
                return headers
            assert line, "Unexpected EOF"
            key, value = line.decode().split(":", 1)
            headers[key.lower()] = value.strip()

    def frame(self):
        assert self.file.readline() == b"--frame\r\n"
        h = self.headers()
        assert h["content-type"] == "image/jpeg"
        data = self.file.read(int(h["content-length"]))
        assert self.file.read(2) == b"\r\n"
        return data

    def close(self):
        self.file.close()
        self.socket.close()


def run(executable):
    with tempfile.TemporaryDirectory(prefix="iris-test-") as directory:
        root = Path(directory)
        video, control, http = free_port(), free_port(), free_port(True)
        while video == control:
            control = free_port()
        cfg = root / "config.ini"
        cfg.write_text(f"[udp]\nhost=127.0.0.1\nport={video}\nframe_timeout_ms=100\n"
                       f"[http]\nport={http}\n[discovery]\nport={control}\nstate_file={root / 'pairings.txt'}\n"
                       "[stream]\nmax_fps=90\n[monitoring]\nstats_interval_seconds=1\noffline_ms=1000\n")
        log_path = root / "bridge.log"
        flags = subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0
        process = None
        streams = []
        udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        udp.bind(("127.0.0.1", 0))
        udp.settimeout(0.2)

        def start(logfile):
            p = subprocess.Popen([str(executable), "--config", str(cfg), "--json"], stdout=logfile, stderr=logfile, creationflags=flags)
            for _ in range(100):
                if p.poll() is not None:
                    raise AssertionError("Bridge failed to start")
                try:
                    with socket.create_connection(("127.0.0.1", http), timeout=0.1):
                        return p
                except OSError:
                    time.sleep(0.03)
            p.kill()
            raise AssertionError("Startup timeout")

        def stop(p):
            p.send_signal(signal.CTRL_BREAK_EVENT if os.name == "nt" else signal.SIGINT)
            p.wait(timeout=5)
            assert p.returncode == 0, "Unclean shutdown"

        def register(camera, boot=1, server=0):
            for _ in range(10):
                udp.sendto(packet(1, camera, boot=boot, server=server), ("127.0.0.1", control))
                try:
                    data, _ = udp.recvfrom(2048)
                    h = HEADER.unpack(data[:48])
                    assert h[2] == 2 and h[4] == camera and h[5] == boot
                    assert struct.unpack("!HH", data[48:]) == (video, 90)
                    return h[-1]
                except socket.timeout:
                    continue
            raise AssertionError("Registration timeout")

        def send_frame(camera, frame, image, server, boot=1, order=None):
            parts = [image[i:i+1200] for i in range(0, len(image), 1200)]
            for i in range(len(parts)) if order is None else order:
                udp.sendto(packet(3, camera, boot, frame, parts[i], i, len(parts), len(image), server), ("127.0.0.1", video))

        # Structural JPEG fixtures: verify exact byte transport, not JPEG decoding.
        images = [b"\xff\xd8" + bytes([i]) * 2800 + b"\xff\xd9" for i in range(1, 4)]
        try:
            with log_path.open("wb") as logfile:
                process = start(logfile)
                server = register(1)
                assert register(2) == server and register(3) == server
                streams = [Stream(http, i) for i in (1, 2, 3)]
                with concurrent.futures.ThreadPoolExecutor() as pool:
                    results = [pool.submit(s.frame) for s in streams]
                    # Interleave three streams, reorder fragments, duplicate fragments.
                    for index in (2, 0, 0, 1):
                        for camera in (1, 2, 3):
                            send_frame(camera, 1, images[camera-1], server, order=[index])
                    for i, result in enumerate(results):
                        assert result.result(timeout=5) == images[i]
                # Incomplete frame expires; delayed fragments cannot resurrect it.
                send_frame(1, 2, images[0], server, order=[0])
                time.sleep(0.15)
                send_frame(1, 2, images[0], server, order=[1, 2])
                send_frame(1, 3, images[1], server)
                assert streams[0].frame() == images[1]
                # A restart resets frame ordering but rejects the old boot/session.
                register(1, boot=2, server=server)
                send_frame(1, 100, images[2], server, boot=1)
                send_frame(1, 1, images[0], server, boot=2)
                assert streams[0].frame() == images[0]
                # Malformed packets and unknown devices cannot replace a published frame.
                udp.sendto(b"OIRB" + b"\xff" * 1500, ("127.0.0.1", video))
                send_frame(99, 1, images[2], server)
                send_frame(2, 4, images[0], server + 1)
                send_frame(2, 5, images[1], server)
                assert streams[1].frame() == images[1]
                time.sleep(1.2)
                register(1, boot=2, server=server)
                send_frame(1, 2, images[2], server, boot=2)
                assert streams[0].frame() == images[2]
                # A non-reading HTTP consumer must not stall a healthy consumer.
                slow = Stream(http, 3, slow=True)
                streams.append(slow)
                large = b"\xff\xd8" + b"s" * 16000 + b"\xff\xd9"
                for i in range(80):
                    if i % 20 == 0:
                        register(3, server=server)
                    send_frame(3, 10+i, large, server)
                    assert streams[2].frame() == large
                    time.sleep(0.025)
                # Server deadline closes slow socket; remaining kernel bytes are finite.
                assert len(slow.file.read()) < len(large) * 80
                slow.close()
                streams.pop()
                for s in streams:
                    s.close()
                streams = []
                stop(process)
                persisted = (root / "pairings.txt").read_text()
                assert "camera 0000000000000001 1 unassigned" in persisted
                # PC restart and camera registration in reverse order preserve endpoints.
                process = start(logfile)
                for camera in (3, 2, 1):
                    assert register(camera, boot=3, server=server) == server
                streams = [Stream(http, 1)]
                send_frame(1, 1, images[0], server, boot=3)
                assert streams[0].frame() == images[0]
                streams[0].close()
                streams = []
                stop(process)
                # Pairing allowlist rejects a valid HELLO from a new ID.
                cfg.write_text(cfg.read_text().replace("[discovery]\n", "[discovery]\nauto_pair=false\n"))
                state_path = root / "pairings.txt"
                state_path.write_text("\n".join(line for line in state_path.read_text().splitlines()
                                                if not line.startswith("camera 0000000000000003 ")) + "\n")
                process = start(logfile)
                udp.sendto(packet(1, 4), ("127.0.0.1", control))
                try:
                    udp.recvfrom(2048)
                    raise AssertionError("Allowlist accepted unknown camera")
                except socket.timeout:
                    pass
                assert register(1, boot=4, server=server) == server
                # Collision is rejected while the old source's heartbeat lease is live.
                migrated = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                migrated.bind(("127.0.0.2", 0))
                migrated.settimeout(0.3)
                try:
                    migrated.sendto(packet(1, 1, boot=4, server=server), ("127.0.0.1", control))
                    try:
                        migrated.recvfrom(2048)
                        raise AssertionError("Active camera ID collision accepted")
                    except socket.timeout:
                        pass
                    time.sleep(6.1)
                    migrated.sendto(packet(1, 1, boot=4, server=server), ("127.0.0.1", control))
                    ack, _ = migrated.recvfrom(2048)
                    assert HEADER.unpack(ack[:48])[-1] == server
                    streams = [Stream(http, 1)]
                    small = b"\xff\xd8migrated\xff\xd9"
                    migrated.sendto(packet(3, 1, boot=4, frame=1, payload=small,
                                           count=1, total=len(small), server=server), ("127.0.0.1", video))
                    assert streams[0].frame() == small
                    streams[0].close()
                    streams = []
                finally:
                    migrated.close()
                stop(process)
            text = log_path.read_text()
            assert "OFFLINE" in text and "RECONNECTED" in text and "STATS" in text
            print("Integration passed: 3 streams, reorder/duplicates/loss, timeout, reboot, invalid packets, slow HTTP client, persistent endpoints, clean shutdown")
        except Exception:
            if process and process.poll() is None:
                process.kill()
                process.wait()
            print(log_path.read_text())
            raise
        finally:
            for s in streams:
                s.close()
            udp.close()
            if process and process.poll() is None:
                process.kill()
                process.wait()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("executable", type=Path)
    run(parser.parse_args().executable.resolve())
