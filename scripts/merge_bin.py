"""PlatformIO post-build hook for `pio run -e release`.

Merges bootloader + partition table + boot_app0 + app into one image that the
browser web flasher writes at offset 0, saves it as
docs/firmware/immich-frame.bin (served by GitHub Pages) and stamps the
firmware version into docs/manifest.json.
"""
import json
import re
import subprocess
from pathlib import Path

Import("env")  # noqa: F821  (injected by PlatformIO)

board = env.BoardConfig()  # noqa: F821
PROJECT_DIR = Path(env["PROJECT_DIR"])  # noqa: F821
DOCS = PROJECT_DIR / "docs"
OUT_BIN = DOCS / "firmware" / "immich-frame.bin"


def fw_version():
    for flag in env.get("CPPDEFINES", []):  # noqa: F821
        if isinstance(flag, tuple) and flag[0] == "FW_VERSION":
            return str(flag[1]).strip('\\"')
    m = re.search(r'FW_VERSION=\?"([^"\]+)', " ".join(env.get("BUILD_FLAGS", [])))  # noqa: F821
    return m.group(1) if m else "dev"


def merge_and_publish(*_args, **_kwargs):
    build_dir = Path(env.subst("$BUILD_DIR"))  # noqa: F821
    boot_app0 = Path(env.subst("$PROJECT_PACKAGES_DIR")) / (  # noqa: F821
        "framework-arduinoespressif32/tools/partitions/boot_app0.bin")
    OUT_BIN.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([
        env.subst("$PYTHONEXE"), env.subst("$OBJCOPY"),  # noqa: F821
        "--chip", board.get("build.mcu", "esp32"), "merge_bin",
        "-o", str(OUT_BIN),
        "--flash_mode", board.get("build.flash_mode", "dio"),
        "--flash_freq", str(board.get("build.f_flash", "40000000L")).replace("000000L", "m"),
        "--flash_size", board.get("upload.flash_size", "4MB"),
        "0x1000", str(build_dir / "bootloader.bin"),
        "0x8000", str(build_dir / "partitions.bin"),
        "0xe000", str(boot_app0),
        "0x10000", str(build_dir / (env.subst("$PROGNAME") + ".bin")),  # noqa: F821
    ], check=True)

    manifest = DOCS / "manifest.json"
    m = json.loads(manifest.read_text()) if manifest.exists() else {}
    m.update({
        "name": "Immich Frame",
        "version": fw_version(),
        "new_install_prompt_erase": True,
        "builds": [{"chipFamily": "ESP32", "parts": [{"path": "firmware/immich-frame.bin", "offset": 0}]}],
    })
    manifest.write_text(json.dumps(m, indent=2) + "\n")
    print("[merge_bin] docs/firmware/%s  version %s, %d KB" % (OUT_BIN.name, m["version"], OUT_BIN.stat().st_size // 1024))


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", merge_and_publish)  # noqa: F821
