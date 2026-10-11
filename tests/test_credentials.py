"""Use a disposable D-Bus session and keyring, never the user's stored passwords."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]

def isolated():
    env = os.environ | {"QT_QPA_PLATFORM": "offscreen", "QT_QUICK_BACKEND": "software", "QT_QPA_PLATFORMTHEME": "", "QT_STYLE_OVERRIDE": ""}
    directory = Path(env["XDG_RUNTIME_DIR"]) / "keyring"
    daemon_log = tempfile.TemporaryFile(mode="w+")
    daemon = subprocess.Popen(["gnome-keyring-daemon", "--foreground", "--unlock", "--components=secrets", f"--control-directory={directory}"],
                              env=env, stdin=subprocess.PIPE, stdout=daemon_log, stderr=daemon_log)
    daemon.stdin.write(b"disposable-test-keyring\n")
    daemon.stdin.close()
    try:
        for _ in range(100):
            probe = subprocess.run(["gdbus", "call", "--session", "--dest", "org.freedesktop.DBus", "--object-path", "/org/freedesktop/DBus", "--method", "org.freedesktop.DBus.NameHasOwner", "org.freedesktop.secrets"],
                                   env=env, capture_output=True, text=True, timeout=3)
            if "true" in probe.stdout:
                break
            time.sleep(.05)
        else:
            daemon_log.seek(0)
            raise AssertionError("Disposable keyring did not start: " + daemon_log.read())
        subprocess.run([str(ROOT / "build/relay"), "--test-credentials"], env=env, check=True, timeout=15)
        subprocess.run([sys.executable, str(ROOT / "tests/test_vnc.py"), "remember", "remember-rejected"], env=env, check=True, timeout=35)
        unavailable = subprocess.run([str(ROOT / "build/relay"), "--test-credentials"],
                                     env=env | {"DBUS_SESSION_BUS_ADDRESS": "unix:path=/tmp/relay-no-such-session-bus"},
                                     capture_output=True, text=True, timeout=15)
        assert unavailable.returncode == 7, (unavailable.returncode, unavailable.stderr)
        print("PASS unavailable keyring: explicit error, no plaintext fallback")
    finally:
        daemon.terminate()
        daemon.wait(timeout=5)
        daemon_log.close()

if "--isolated" in sys.argv:
    isolated()
else:
    with tempfile.TemporaryDirectory(prefix="relay-keyring-") as temporary:
        base = Path(temporary)
        for name in ("config", "data", "runtime", "home"):
            (base / name).mkdir(mode=0o700)
        env = os.environ | {"XDG_CONFIG_HOME": str(base / "config"), "XDG_DATA_HOME": str(base / "data"), "XDG_RUNTIME_DIR": str(base / "runtime"), "HOME": str(base / "home")}
        env.pop("GNOME_KEYRING_CONTROL", None)
        subprocess.run(["dbus-run-session", "--", sys.executable, __file__, "--isolated"], env=env, check=True, timeout=60)
