"""Render only synthetic Relay windows; no desktop or remote content is captured."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]

def run(output):
    output.mkdir(parents=True, exist_ok=True)
    for theme in ("dark", "light"):
        for size in ("1180x780", "880x620", "640x440"):
            for dialog in ("home", "login", "preferences", "help"):
                with tempfile.TemporaryDirectory(prefix="relay-ui-config-") as config:
                    settings = Path(config) / "Relay/Relay.conf"
                    settings.parent.mkdir()
                    settings.write_text("[General]\nhost=mac.example.ts.net\nusername=utente\ndisplayName=Mac Studio\n" + f"dark={'true' if theme == 'dark' else 'false'}\n")
                    image = output / f"{theme}-{size}-{dialog}.png"
                    args = [str(ROOT / "build/relay"), "--test-ui", str(image), "--ui-size", size]
                    if dialog != "home":
                        args += ["--ui-dialog", dialog]
                    result = subprocess.run(args, env=os.environ | {
                        "QT_QPA_PLATFORM": "offscreen", "QT_QUICK_BACKEND": "software",
                        "QT_QPA_PLATFORMTHEME": "", "QT_STYLE_OVERRIDE": "", "XDG_CONFIG_HOME": config,
                    }, capture_output=True, text=True, timeout=10)
                    assert result.returncode == 0, (args, result.stdout, result.stderr)
                    assert "qrc:/ui/" not in result.stderr, result.stderr
                    assert image.stat().st_size > 1000, image
        print(f"PASS {theme}: home and 3 dialogs at 1180x780, 880x620 and 640x440")

if len(sys.argv) > 1:
    run(Path(sys.argv[1]))
else:
    with tempfile.TemporaryDirectory(prefix="relay-ui-") as output:
        run(Path(output))
