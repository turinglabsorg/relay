"""Exercise the actual LibVNCClient connection and Qt framebuffer with a local RFB peer."""
import hashlib
import os
from pathlib import Path
import select
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

APPLE_PRIME = int(
    "FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD129024E088A67CC74"
    "020BBEA63B139B22514A08798E3404DDEF9519B3CD3A431B302B0A6DF25F1437"
    "4FE1356D6D51C245E485B576625E7EC6F44C42E9A637ED6B0BFF5CB6F406B7ED"
    "EE386BFB5A899FA5AE9F24117C4B1FE649286651ECE65381FFFFFFFFFFFFFFFF", 16)

def run_case(mode):
    low = mode == "low-bandwidth"
    apple_stats = {"arms": 0, "requests": 0, "pushed": 0}
    initial_width = 5 if low else 4
    errors, inputs = [], []
    responsive = {"before_complete": False, "data": bytearray()}
    listener = socket.socket()
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)
    port = listener.getsockname()[1]

    def peer():
        try:
            # Relay first probes the greeting on a throwaway connection to
            # detect Apple 003.889 servers; like a real server, accept the
            # real connection that follows it.
            conn = None
            while conn is None:
                candidate, _ = listener.accept()
                candidate.settimeout(12)
                candidate.sendall(b"RFB 003.008\n")
                try:
                    assert receive(candidate, 12) == b"RFB 003.008\n"
                    conn = candidate
                except (EOFError, AssertionError):
                    candidate.close()
            with conn:
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
                            if mode in ("resize", "remember", "view-input"):
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
                            elif mode == "responsive-input":
                                # One update with many raw tiles, dribbled out.
                                # A responsive client flushes queued pointer
                                # input between rects instead of waiting for
                                # the whole update to arrive.
                                tiles = []
                                for ty in (0, 2, 4):
                                    for tx in (0, 2, 4, 6):
                                        tiles.append(struct.pack("!HHHHi", tx, ty, 2, 2, 0)
                                                     + pixel_bytes(0x1234AB) * 4)
                                header = struct.pack("!BBH", 0, 0, 1 + len(tiles))
                                header += struct.pack("!HHHHi", 0, 0, 8, 6, -223)
                                pieces = [header] + tiles
                                # Drain input queued by the first frame so
                                # only input sent during this update counts.
                                time.sleep(.12)
                                while select.select([conn], [], [], 0)[0]:
                                    chunk = conn.recv(4096)
                                    if not chunk:
                                        break
                                    while chunk:
                                        if chunk[0] == 5 and len(chunk) >= 6:
                                            mask, x, y = struct.unpack("!BHH", chunk[1:6])
                                            inputs.append(("pointer", x, y, mask))
                                            chunk = chunk[6:]
                                        elif chunk[0] == 3 and len(chunk) >= 10:
                                            chunk = chunk[10:]
                                        elif chunk[0] == 4 and len(chunk) >= 8:
                                            chunk = chunk[8:]
                                        else:
                                            chunk = b""
                                for index, piece in enumerate(pieces):
                                    conn.sendall(piece)
                                    if index < len(pieces) - 1:
                                        time.sleep(.04)
                                        ready, _, _ = select.select([conn], [], [], 0)
                                        if ready:
                                            chunk = conn.recv(4096)
                                            if chunk:
                                                responsive["data"] += chunk
                                                if chunk[0] == 5:
                                                    responsive["before_complete"] = True
                                pending = bytes(responsive["data"])
                                while pending:
                                    if pending[0] == 5 and len(pending) >= 6:
                                        mask, x, y = struct.unpack("!BHH", pending[1:6])
                                        inputs.append(("pointer", x, y, mask))
                                        pending = pending[6:]
                                    elif pending[0] == 3 and len(pending) >= 10:
                                        pending = pending[10:]
                                    elif pending[0] == 4 and len(pending) >= 8:
                                        pending = pending[8:]
                                    else:
                                        break
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

    def peer_apple():
        try:
            conn, _ = listener.accept()
            with conn:
                conn.settimeout(12)
                conn.sendall(b"RFB 003.889\n")
                assert receive(conn, 12) == b"RFB 003.889\n"
                conn.sendall(b"\x01\x1e")
                assert receive(conn, 1) == b"\x1e"
                key_len = 128
                server_priv = int.from_bytes(os.urandom(32), "big")
                conn.sendall(struct.pack("!HH", 2, key_len)
                             + APPLE_PRIME.to_bytes(key_len, "big")
                             + pow(2, server_priv, APPLE_PRIME).to_bytes(key_len, "big"))
                blob = receive(conn, 128)
                client_pub = int.from_bytes(receive(conn, key_len), "big")
                secret = pow(client_pub, server_priv, APPLE_PRIME).to_bytes(key_len, "big")
                plain = subprocess.run(
                    ["openssl", "enc", "-d", "-aes-128-ecb", "-K", hashlib.md5(secret).hexdigest(), "-nopad"],
                    input=blob, capture_output=True).stdout
                assert plain[0:5] == b"relay" and plain[5] == 0, plain[:16]
                assert plain[64:71] == b"segreto" and plain[71] == 0
                conn.sendall(struct.pack("!I", 0))
                assert receive(conn, 1) == b"\x81"
                name = bytes(22) + b"Mac Test"
                conn.sendall(struct.pack("!HH", 4, 3)
                             + struct.pack("!BBBBHHHBBBxxx", 32, 24, 0, 1, 255, 255, 255, 16, 8, 0)
                             + struct.pack("!I", len(name)) + name)
                codec = mode == "apple-codec"
                compressor = zlib.compressobj()
                def rgb555(rgb):
                    word = ((rgb >> 19) & 31) << 10 | ((rgb >> 11) & 31) << 5 | ((rgb >> 3) & 31)
                    return struct.pack(">H", word)
                def zlib_rect(w, h, rgb, x=0, y=0):
                    encoding = 0x3ea if codec else 6
                    pixels = (rgb555(rgb) if codec else pixel_bytes(rgb)) * (w * h)
                    chunk = compressor.compress(pixels) + compressor.flush(zlib.Z_SYNC_FLUSH)
                    return struct.pack("!HHHHi", x, y, w, h, encoding) + struct.pack("!I", len(chunk)) + chunk
                zrle_stream = zlib.compressobj()
                def zrle_rect(w, h, rgb, x=0, y=0):
                    # One solid ZRLE tile. Qualità CPIXEL is little-endian B, G, R.
                    raw = bytes((1, rgb & 0xFF, (rgb >> 8) & 0xFF, (rgb >> 16) & 0xFF))
                    chunk = zrle_stream.compress(raw) + zrle_stream.flush(zlib.Z_SYNC_FLUSH)
                    return struct.pack("!HHHHi", x, y, w, h, 16) + struct.pack("!I", len(chunk)) + chunk
                record = struct.pack("!ddI", 1.0, 1.0, 1) + struct.pack("!HHHH", 0, 0, 6, 8) \
                    + struct.pack("!HHHH", 0, 0, 6, 8) + struct.pack("!I", 1) + bytes(16)
                layout_payload = struct.pack("!HHHHHIIH", 5, 8, 6, 8, 6, 1, 4, 1) + record
                layout = struct.pack("!HHHHi", 0, 0, 0, 0, 0x451) + struct.pack("!H", len(layout_payload)) + layout_payload
                stage = 0
                while True:
                    message = receive(conn, 1)[0]
                    if message == 0x21:
                        receive(conn, 65)
                    elif message == 0x0a:
                        receive(conn, 3)
                    elif message == 0:
                        receive(conn, 19)
                    elif message == 2:
                        head = receive(conn, 3)
                        count = struct.unpack("!H", head[1:3])[0]
                        offered = struct.unpack("!" + "i" * count, receive(conn, 4 * count))
                        assert offered.index(16) < offered.index(6) < offered.index(0), offered
                        assert 1 in offered, offered
                        if codec:
                            assert offered.index(16) < offered.index(0x3ea) < offered.index(6), offered
                            assert 0x3f3 not in offered, offered
                        else:
                            assert offered.index(0x3f3) < offered.index(16), offered
                            assert 0x3ea not in offered, offered
                        conn.sendall(struct.pack("!BBH", 0, 0, 1) + zlib_rect(4, 3, 0xEF4A32))
                        apple_stats["pushed"] += 1
                        time.sleep(0.2)
                        conn.sendall(struct.pack("!BBH", 0, 0, 1) + layout)
                        stage = 1
                    elif message == 3:
                        receive(conn, 9)
                        apple_stats["requests"] += 1
                    elif message == 9:
                        arm = receive(conn, 15)
                        assert arm[1:3] == b"\x00\x01" and arm[3:7] == bytes(4), arm
                        apple_stats["arms"] += 1
                        if stage == 1:
                            stage = 2
                            if codec:
                                # Fill, punch out the sampled pixel, then CopyRect it
                                # back from its neighbour. A skipped copy leaves black.
                                body = zlib_rect(8, 6, 0x1234AB) + zlib_rect(1, 1, 0)
                                body += struct.pack("!HHHHiHH", 0, 0, 1, 1, 1, 1, 0)
                                conn.sendall(struct.pack("!BBH", 0, 0, 3) + body)
                            else:
                                conn.sendall(struct.pack("!BBH", 0, 0, 1) + zrle_rect(8, 6, 0x1234AB))
                            apple_stats["pushed"] += 1
                    elif message == 4:
                        data = receive(conn, 7)
                        inputs.append(("key", struct.unpack("!I", data[3:])[0], data[0]))
                    elif message == 5:
                        mask, x, y = struct.unpack("!BHH", receive(conn, 5))
                        inputs.append(("pointer", x, y, mask))
                    else:
                        raise AssertionError(f"unexpected Apple message {message}")
        except EOFError:
            pass
        except Exception as error:
            errors.append(error)
        finally:
            listener.close()

    thread = threading.Thread(target=peer_apple if mode in ("apple", "apple-codec") else peer, daemon=True)
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
        if mode == "responsive-input":
            arguments.append("--test-input-during-frame")
        if mode == "view-input":
            arguments.append("--test-view-input")
        if mode in ("apple", "apple-codec"):
            arguments.append("--test-apple")
        if low or mode == "apple-codec":
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
    expected = 0 if mode in ("resize", "buffered-burst", "slow-frame", "zlib-cursor", "low-bandwidth", "cancel", "remember", "responsive-input", "view-input", "apple", "apple-codec") else 4
    assert result.returncode == expected, (mode, result.returncode, result.stdout, result.stderr)
    if mode == "cancel":
        assert "CANCELLED" in result.stdout, result.stdout
    if mode == "zlib-cursor":
        assert "CURSOR 2x2 1,0 #abcdef 0" in result.stdout, result.stdout
    if low:
        assert "CURSOR 3x2 1,0 #adcfef 0" in result.stdout, result.stdout
        assert "Relay profile: Fluido / RGB565" in result.stderr, result.stderr
    if mode == "responsive-input":
        assert responsive["before_complete"], ("pointer input waited for the full framebuffer update", responsive, inputs, result.stdout, result.stderr)
        assert ("pointer", 3, 2, 1) in inputs and ("pointer", 3, 2, 0) in inputs, inputs
    if mode in ("resize", "buffered-burst", "slow-frame", "zlib-cursor", "low-bandwidth", "remember", "responsive-input", "view-input", "apple", "apple-codec"):
        assert "RENDER pixels and letterbox after window resize" in result.stdout, result.stdout
        initial = "4x3 #ef4931" if mode == "apple-codec" else "5x3 #ef4931" if low else "4x3 #ef4a32"
        assert f"FRAME {initial}" in result.stdout, result.stdout
        size = "64x6" if mode == "slow-frame" else "9x6" if low else "8x6"
        color = "#1030ad" if mode == "apple-codec" else "#1034ad" if low else "#1234ab"
        assert f"FRAME {size} {color}" in result.stdout, result.stdout
        if mode == "view-input":
            # Events go through RemoteView (window delivery), not Session directly:
            # a left click at the image centre and an "a" keypress must arrive.
            assert "VIEWINPUT view=1" in result.stdout, result.stdout
            assert ("pointer", 4, 3, 1) in inputs and ("pointer", 4, 3, 0) in inputs, inputs
            assert not any(i[0] == "pointer" and i[3] == 4 for i in inputs), inputs
            # Each physical click is one press: a duplicated press is a double-click.
            assert inputs.count(("pointer", 4, 3, 1)) == 2, inputs
            # Super arrives on the Mac as Control (0xffe3); a lost release must be
            # healed before the next click, or macOS sees Ctrl+click (right click).
            assert ("key", 0xFFE3, 1) in inputs and ("key", 0xFFE3, 0) in inputs, inputs
            meta_down = inputs.index(("key", 0xFFE3, 1))
            meta_up = inputs.index(("key", 0xFFE3, 0))
            clicks_after = [i for i in range(meta_down, len(inputs)) if inputs[i][0] == "pointer" and inputs[i][3] == 1]
            assert clicks_after and meta_up < clicks_after[0], inputs
        else:
            assert ("pointer", 2, 1, 1) in inputs and ("pointer", 2, 1, 0) in inputs, inputs
        assert ("key", ord("a"), 1) in inputs and ("key", ord("a"), 0) in inputs, inputs
        if mode in ("apple", "apple-codec"):
            # The session arms Apple's pushed updates and re-arms after the
            # layout, while asking for at most an opening and a repaint frame.
            assert apple_stats["arms"] >= 2 and apple_stats["pushed"] >= 2, apple_stats
            assert apple_stats["requests"] <= 2, apple_stats
    print(f"PASS {mode}: protocol negotiation, framebuffer and event loop")

with tempfile.TemporaryDirectory(prefix="relay-unit-") as config:
    subprocess.run([str(ROOT / "build/relay"), "--self-test"], env=os.environ | {"QT_QPA_PLATFORM":"offscreen", "QT_QPA_PLATFORMTHEME":"", "QT_STYLE_OVERRIDE":"", "XDG_CONFIG_HOME":config}, check=True)
for mode in (sys.argv[1:] or ("resize", "buffered-burst", "slow-frame", "responsive-input", "view-input", "apple", "apple-codec", "zlib-cursor", "low-bandwidth", "cancel", "disconnect", "oversized", "auth-rejected")):
    run_case(mode)
