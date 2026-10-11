#!/usr/bin/env python3
"""Install this built app and its launcher for the current user, without root."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parent
home = Path.home()
data = Path(os.environ.get("XDG_DATA_HOME", str(home / ".local/share")))
binary = home / ".local/bin/relay"
icon = data / "icons/hicolor/scalable/apps/io.relay.svg"
desktop = data / "applications/io.relay.desktop"
for file in (binary, icon, desktop):
    file.parent.mkdir(parents=True, exist_ok=True)
descriptor, temporary = tempfile.mkstemp(prefix=".relay-install-", dir=binary.parent)
os.close(descriptor)
temporary = Path(temporary)
try:
    shutil.copy2(root / "build/relay", temporary)
    temporary.chmod(0o755)
    os.replace(temporary, binary)
finally:
    temporary.unlink(missing_ok=True)
shutil.copy2(root / "assets/relay.svg", icon)
desktop.write_text(
    "[Desktop Entry]\nType=Application\nName=Relay\n"
    "Comment=Il tuo Mac, un po’ più vicino\n"
    f'Exec="{binary}" -platform wayland\nIcon={icon}\n'
    "Terminal=false\nCategories=Network;RemoteAccess;\n"
    "Keywords=Mac;Tailscale;VNC;desktop;\nStartupNotify=true\n"
)
if shutil.which("desktop-file-validate"):
    subprocess.run(["desktop-file-validate", str(desktop)], check=True)
if shutil.which("update-desktop-database"):
    subprocess.run(["update-desktop-database", str(desktop.parent)], check=True)
print(f"Installed Relay: {binary}\nLauncher: {desktop}")
