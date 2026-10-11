"""Exercise the actual LibVNCClient connection and Qt framebuffer with a local RFB peer."""
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import zlib

ROOT = Path(__file__).resolve().parents[1]

def receive(sock, count):
    data = b""
    while len(data) < count:
        part = sock.recv(count - len(data))
        if not part:
            raise EOFError("client disconnected")
        data += part
    return data

def pixel_bytes(rgb, low=False):
    if low:
        value = ((rgb >> 19) & 31) << 11 | ((rgb >> 10) & 63) << 5 | ((rgb >> 3) & 31)
        return struct.pack("<H", value)
    return struct.pack("<I", rgb)

def framebuffer(width, height, rgb, resize=False, low=False):
    count = 2 if resize else 1
    result = struct.pack("!BBH", 0, 0, count)
    if resize:
        result += struct.pack("!HHHHi", 0, 0, width, height, -223)
    result += struct.pack("!HHHHi", 0, 0, width, height, 0)
    result += pixel_bytes(rgb, low) * width * height
    return result

def run_case(mode):
    low = mode == "low-bandwidth"
    initial_width = 5 if low else 4
    errors, inputs = [], []
    listener = socket.socket()
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)
    port = listener.getsockname()[1]

    def peer():
        try:
            conn, _ = listener.accept()
            with conn:
                conn.settimeout(12)
                conn.sendall(b"RFB 003.008\n")
                assert receive(conn, 12) == b"RFB 003.008\n"
                if mode in ("auth-rejected", "remember-rejected"):
                    conn.sendall(b"\x01\x02")
                    assert receive(conn, 1) == b"\x02"
                    conn.sendall(bytes(16))
                    receive(conn, 16)
                    reason = b"Authentication rejected by test server"
                    conn.sendall(struct.pack("!II", 1, len(reason)) + reason)
                    return
                conn.sendall(b"\x01\x01")  # security type None, only inside this local test
                assert receive(conn, 1) == b"\x01"
                conn.sendall(struct.pack("!I", 0))
                receive(conn, 1)
                pixel_format = struct.pack("!BBBBHHHBBBxxx", 32, 24, 0, 1, 255, 255, 255, 16, 8, 0)
                name = b"Relay Test Desktop"
                conn.sendall(struct.pack("!HH", initial_width, 3) + pixel_format + struct.pack("!I", len(name)) + name)
                step = 0
                while True:
                    message = receive(conn, 1)[0]
                    if message == 0:
                        fmt = receive(conn, 19)[3:]
                        assert fmt[0] == (16 if low else 32), fmt
                        assert tuple(fmt[10:13]) == ((11,5,0) if low else (16,8,0)), fmt
                        assert struct.unpack("!HHH",fmt[4:10]) == ((31,63,31) if low else (255,255,255)), fmt
                    elif message == 2:
                        _, count = struct.unpack("!BH", receive(conn, 3))
                        encodings = struct.unpack("!" + "i" * count, receive(conn, 4 * count))
                        assert 0 in encodings and -223 in encodings
                        assert 6 in encodings, "Zlib must be offered to the Mac server"
                        assert -239 in encodings, "The server must be asked for a local cursor"
                    elif message == 3:
                        receive(conn, 9)
                        if step == 0:
                            if mode == "cancel":
                                conn.sendall(framebuffer(4, 3, 0xEF4A32)[:18])
                                while conn.recv(1024):
                                    pass
                                return
                            conn.sendall(framebuffer(initial_width, 3, 0xEF4A32, low=low))
                            step = 1
                        elif step == 1:
                            # Wait so Qt sees the first frame as well as the resize.
                            time.sleep(.15)
                            if mode in ("resize", "remember"):
                                conn.sendall(framebuffer(8, 6, 0x1234AB, resize=True))
                            elif mode == "buffered-burst":
                                # Bell and a frame arrive together; the decoder prefetches
                                # the frame, so select() alone sees no more socket traffic.
                                conn.sendall(b'\x02' + framebuffer(8, 6, 0x1234AB, resize=True))
                            elif mode == "slow-frame":
                                packet = framebuffer(64, 6, 0x1234AB, resize=True)
                                conn.sendall(packet[:28])
                                for i in range(28,len(packet),4):
                                    conn.sendall(packet[i:i+4])
                                    time.sleep(.003)
                            elif mode in ("zlib-cursor", "low-bandwidth"):
                                # A rich cursor with a transparent pixel followed by
                                # a compressed resized desktop in the same packet.
                                packet = struct.pack("!BBH", 0, 0, 3)
                                cursor_width = 3 if low else 2
                                final_width = 9 if low else 8
                                packet += struct.pack("!HHHHi", 1, 0, cursor_width, 2, -239)
                                packet += pixel_bytes(0xABCDEF, low) * cursor_width * 2 + b'\x80\xc0'
                                packet += struct.pack("!HHHHi", 0, 0, final_width, 6, -223)
                                packet += struct.pack("!HHHHi", 0, 0, final_width, 6, 6)
                                compressor = zlib.compressobj()
                                pixels = pixel_bytes(0x1234AB, low) * final_width * 6
                                if low:
                                    assert len(pixels) == final_width * 6 * 2
                                compressed = compressor.compress(pixels) + compressor.flush(zlib.Z_SYNC_FLUSH)
                                packet += struct.pack("!I", len(compressed)) + compressed
                                conn.sendall(packet)
                            elif mode == "oversized":
                                conn.sendall(struct.pack("!BBHHHHHi",0,0,1,0,0,65535,65535,-223))
                            elif mode == "disconnect":
                                return
                            step = 2
                    elif message == 4:
                        data = receive(conn, 7)
                        down, key = data[0], struct.unpack("!I", data[3:])[0]
                        inputs.append(("key", key, down))
                    elif message == 5:
                        mask, x, y = struct.unpack("!BHH", receive(conn, 5))
                        inputs.append(("pointer", x, y, mask))
                    elif message == 6:
                        data = receive(conn, 7)
                        receive(conn, struct.unpack("!I", data[3:])[0])
                    else:
                        raise AssertionError(f"unexpected message {message}")
        except EOFError:
            pass
        except Exception as error:
            errors.append(error)
        finally:
            listener.close()

    thread = threading.Thread(target=peer, daemon=True)
    thread.start()
    with tempfile.TemporaryDirectory(prefix="relay-test-") as config:
        platform = os.environ.get("RELAY_TEST_PLATFORM", "offscreen")
        env = os.environ | {"QT_QPA_PLATFORM": platform, "QT_QPA_PLATFORMTHEME": "", "QT_STYLE_OVERRIDE": "", "XDG_CONFIG_HOME": config}
        if platform == "offscreen":
            env["QT_QUICK_BACKEND"] = "software"
        arguments = [str(ROOT / "build/relay"), "--test-connect", f"127.0.0.1:{port}"]
        if mode == "cancel":
            arguments.append("--test-cancel")
        if mode in ("remember", "remember-rejected"):
            arguments.append("--test-remember")
        if low:
            arguments.append("--test-low-bandwidth")
        result = subprocess.run(arguments, env=env, capture_output=True, text=True, timeout=15)
        for file in Path(config).rglob("*"):
            if file.is_file():
                assert b"Synthetic Relay" not in file.read_bytes(), "Password leaked into settings"
        if mode == "remember":
            assert "PASS consent, successful-authentication save, reload and forget" in result.stdout, (result.stdout, result.stderr)
    thread.join(3)
    assert not errors, errors
    assert "ReferenceError" not in result.stderr and "TypeError" not in result.stderr, result.stderr
    expected = 0 if mode in ("resize", "buffered-burst", "slow-frame", "zlib-cursor", "low-bandwidth", "cancel", "remember") else 4
    assert result.returncode == expected, (mode, result.returncode, result.stdout, result.stderr)
    if mode == "cancel":
        assert "CANCELLED" in result.stdout, result.stdout
    if mode == "zlib-cursor":
        assert "CURSOR 2x2 1,0 #abcdef 0" in result.stdout, result.stdout
    if low:
        assert "CURSOR 3x2 1,0 #adcfef 0" in result.stdout, result.stdout
        assert "Relay profile: Fluido / RGB565" in result.stderr, result.stderr
    if mode in ("resize", "buffered-burst", "slow-frame", "zlib-cursor", "low-bandwidth", "remember"):
        assert "RENDER pixels and letterbox after window resize" in result.stdout, result.stdout
        initial = "5x3 #ef4931" if low else "4x3 #ef4a32"
        assert f"FRAME {initial}" in result.stdout, result.stdout
        size = "64x6" if mode == "slow-frame" else "9x6" if low else "8x6"
        color = "#1034ad" if low else "#1234ab"
        assert f"FRAME {size} {color}" in result.stdout, result.stdout
        assert ("pointer", 2, 1, 1) in inputs and ("pointer", 2, 1, 0) in inputs, inputs
        assert ("key", ord("a"), 1) in inputs and ("key", ord("a"), 0) in inputs, inputs
    print(f"PASS {mode}: protocol negotiation, framebuffer and event loop")

with tempfile.TemporaryDirectory(prefix="relay-unit-") as config:
    subprocess.run([str(ROOT / "build/relay"), "--self-test"], env=os.environ | {"QT_QPA_PLATFORM":"offscreen", "QT_QPA_PLATFORMTHEME":"", "QT_STYLE_OVERRIDE":"", "XDG_CONFIG_HOME":config}, check=True)
for mode in (sys.argv[1:] or ("resize", "buffered-burst", "slow-frame", "zlib-cursor", "low-bandwidth", "cancel", "disconnect", "oversized", "auth-rejected")):
    run_case(mode)
