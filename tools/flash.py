#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# /// script
# requires-python = ">=3.10"
# dependencies = ["esptool>=5,<6"]
# ///
"""DragonBreath installer/flasher for the BIGTREETECH Panda Breath (ESP32-C3).

*** READ THIS FIRST ***
Installing DragonBreath OVERWRITES THE ENTIRE FLASH and ERASES the stock firmware.
BIGTREETECH does not publish stock images, so THE FULL BACKUP THIS TOOL TAKES IS
THE ONLY WAY BACK TO STOCK. If you skip the backup or lose the backup file, THERE
IS NO GOING BACK. Store the backup somewhere safe (copy it off this machine).

Backs up the ENTIRE existing flash to a timestamped file BEFORE writing anything,
then flashes the DragonBreath build. The backup is a full 4 MB image, so you can
return to stock:

    # First-time install (back up stock, then flash DragonBreath):
    uv run tools/flash.py

    # Restore a previous backup (e.g. go back to stock):
    uv run tools/flash.py --restore backups/stock-YYYYmmdd-HHMMSS.bin

    # Reset config only (Wi-Fi/token/policy/calibration) — keep the firmware:
    uv run tools/flash.py --erase-nvs

The Panda Breath has no exposed native-USB (GPIO18 is the SSR), so flashing is
over the on-board CH340K USB-C UART bridge. Plug the board's USB-C into your PC;
esptool auto-detects the port (override with --port).

Requires esptool 5 (the "# /// script" block above, PEP 723). `uv run` reads
that block and installs esptool for this script, on Linux, macOS and Windows:
https://docs.astral.sh/uv/getting-started/installation/
Without uv, install the same requirement into the Python that runs this script:
`python3 -m pip install "esptool>=5,<6"`, then `python3 tools/flash.py ...`.
The esptool 4 copy in ESP-IDF 5.x is too old: esptool 5 renamed the commands.
Tested against a V1.0.1 board — verify your board revision first.
"""
import argparse
import datetime
import hashlib
import os
import re
import subprocess
import sys

CHIP = "esp32c3"
FLASH_SIZE = 0x400000  # 4 MB
# NVS (config) partition — matches partitions.csv. Erasing only this wipes Wi-Fi
# credentials, control token, saved policy, and sensor calibration, forcing AP
# re-provisioning on next boot — WITHOUT touching the firmware. The minimal
# "reset my config" fix (same effect as the on-device Power+Auto reset combo).
NVS_OFFSET = 0x9000
NVS_SIZE   = 0x5000    # 20 KB
# DragonBreath image layout — matches partitions.csv (stock Panda layout: otadata
# @ 0xe000, app slot ota_0 @ 0x10000). This is the USB/recovery "factory" write.
IMAGES = [
    ("0x0",      "bootloader/bootloader.bin"),
    ("0x8000",   "partition_table/partition-table.bin"),
    ("0xe000",   "ota_data_initial.bin"),
    ("0x10000",  "dragonbreath.bin"),
]
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def esptool_cmd(port, baud, *args):
    cmd = [sys.executable, "-m", "esptool", "--chip", CHIP]
    if port:
        cmd += ["--port", port]
    cmd += ["--baud", str(baud), *args]
    return cmd


def run(cmd):
    print("  $ " + " ".join(cmd))
    return subprocess.call(cmd)


# Fast -> reliable. Poor USB-serial links (some laptops, long/cheap cables, certain
# CH340 adapters) corrupt bulk transfers at high baud, so an operation that fails is
# retried at the next slower rate automatically. --baud pins a single rate instead.
BAUD_LADDER = [460800, 230400, 115200]


def run_esptool(port, bauds, *args):
    """Run one esptool operation, dropping to a slower baud on failure. esptool
    read-flash/write-flash re-run cleanly (they overwrite the whole target), so a
    failed high-baud attempt is safe to retry slower. Returns (rc, used_baud)."""
    rc = 1
    for i, b in enumerate(bauds):
        if len(bauds) > 1:
            print(f"      [connection speed: {b} baud]")
        rc = run(esptool_cmd(port, b, *args))
        if rc == 0:
            return 0, b
        if i + 1 < len(bauds):
            print(f"      {b} baud failed — retrying at {bauds[i + 1]} baud "
                  f"(slower, more reliable on poor connections)…")
    return rc, bauds[-1]


# The full backup is read in chunks. esptool streams a read as 4 KB frames
# with up to 64 frames in flight, and one lost byte stops the whole read
# ("Corrupt data, expected 0x1000 bytes but received 0xfe6 bytes"). On a
# Panda Breath with a CH340K and the macOS WCH driver, about 1 frame in 100
# lost bytes at 115200, 230400 and 460800 baud, so a single 4 MB read (1024
# frames) almost never completed. esptool checks each read against an MD5
# digest from the chip, so a chunk that reads without error is correct.
# Only a failed chunk is read again.
READ_CHUNK = 0x20000          # 128 KB: 32 frames
CHUNK_TRIES_PER_BAUD = 3


def read_flash_chunked(port, bauds, path, size=None, chunk=None):
    """Read `size` bytes of flash into `path`, `chunk` bytes at a time.

    Each chunk gets CHUNK_TRIES_PER_BAUD tries at each rate of `bauds`. A
    chunk that fails at all rates stops the read and removes `path`.
    Returns the lowest baud rate that a chunk needed, or None on failure."""
    size = FLASH_SIZE if size is None else size
    chunk = READ_CHUNK if chunk is None else chunk
    part = path + ".part"
    total = size // chunk
    slowest = None
    done = False
    try:
        with open(path, "wb") as out:
            for n, offset in enumerate(range(0, size, chunk), start=1):
                baud = read_one_chunk(port, bauds, offset, chunk, part, n, total)
                if baud is None:
                    return None
                slowest = baud if slowest is None else min(slowest, baud)
                with open(part, "rb") as f:
                    out.write(f.read())
        done = True
        return slowest
    finally:
        if os.path.exists(part):
            os.remove(part)
        if not done and os.path.exists(path):
            os.remove(path)


def read_one_chunk(port, bauds, offset, length, part, n, total):
    for b in bauds:
        for attempt in range(1, CHUNK_TRIES_PER_BAUD + 1):
            print(f"      chunk {n}/{total} at {offset:#08x}, {b} baud, try {attempt}")
            if os.path.exists(part):
                os.remove(part)
            # no-reset: the chip stays in the loader. A hard reset boots the app
            # between chunks, and the app can write NVS during the backup.
            rc = run(esptool_cmd(port, b, "--after", "no-reset",
                                 "read-flash", hex(offset), hex(length), part))
            if rc == 0 and os.path.exists(part) and os.path.getsize(part) == length:
                return b
    print(f"ERROR: chunk {n}/{total} at {offset:#08x} failed at all baud rates.")
    return None


ESPTOOL_MAJOR = 5   # the command names below are the esptool 5 names


def esptool_version(output):
    """Return (major, version) from the output of `esptool version`.

    esptool 5 prints "esptool v5.6.0" then "5.6.0". esptool 4 prints
    "esptool.py v4.8.1" then "4.8.1". Other lines (warnings) are ignored.
    Returns (None, None) if no version is in the output."""
    m = re.search(r"(?m)^(?:esptool(?:\.py)? v)?((\d+)\.\d+\S*)\s*$", output)
    return (int(m.group(2)), m.group(1)) if m else (None, None)


def script_command(argv0):
    """The command that runs this script again with uv, for messages.

    A user who downloaded only flash.py runs `uv run flash.py`, not
    `uv run tools/flash.py`. Thus use the path that started this script."""
    return f'uv run "{argv0}"' if " " in argv0 else f"uv run {argv0}"


def check_esptool():
    """Return True if this Python has esptool 5. Else print how to get it."""
    try:
        out = subprocess.run([sys.executable, "-m", "esptool", "version"],
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             text=True, check=False).stdout
    except OSError:
        out = ""
    major, found = esptool_version(out)
    if major == ESPTOOL_MAJOR:
        return True
    if found is None:
        print(f"ERROR: esptool not found for this Python:\n         {sys.executable}")
    else:
        print(f"ERROR: esptool {found} found, but this tool needs esptool "
              f"{ESPTOOL_MAJOR} (the command names changed in esptool 5).")
    print("       Run the tool with uv. uv reads the dependencies at the top of")
    print("       this file and installs esptool for this run:")
    print(f"         {script_command(sys.argv[0])} ...")
    if os.environ.get("IDF_PYTHON_ENV_PATH") or os.environ.get("IDF_PATH"):
        # ESP-IDF 5.x pins esptool 4. esptool 5 in its Python breaks idf.py.
        print("       Do not install esptool 5 into the ESP-IDF Python: ESP-IDF")
        print("       needs its own esptool 4. Use uv, or run from a shell")
        print("       without the ESP-IDF environment.")
    else:
        print("       Or install the requirement into this exact interpreter:")
        print(f'         "{sys.executable}" -m pip install '
              f'"esptool>={ESPTOOL_MAJOR},<{ESPTOOL_MAJOR + 1}"')
    return False


def confirm(prompt):
    try:
        return input(prompt + " [y/N] ").strip().lower() in ("y", "yes")
    except (EOFError, KeyboardInterrupt):
        print()
        return False


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def do_backup(port, bauds, backup_dir):
    """Returns (path, used_baud) on success, or (None, None) on failure.

    The read goes to `<name>.unverified`. Only a file that passes all checks,
    including the on-chip hash, gets its final name `stock-<stamp>.bin`. Thus
    a failed or interrupted backup never looks like a good one."""
    os.makedirs(backup_dir, exist_ok=True)
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    path = os.path.join(backup_dir, f"stock-{stamp}.bin")
    tmp = path + ".unverified"
    print(f"\n[1/3] Backing up the full {FLASH_SIZE // (1024*1024)} MB flash -> {path}")
    print(f"      (this reads the WHOLE chip in {READ_CHUNK // 1024} KB chunks; "
          "a few minutes. Do not unplug.)")
    try:
        baud = read_flash_chunked(port, bauds, tmp)
        if baud is None:
            print("ERROR: backup read failed at all baud rates — NOT flashing. Fix the "
                  "connection (cable/port) and retry.")
            return None, None

        # 1) exact size, 2) not a blank/failed read (real image starts with the ESP
        # magic byte 0xE9), 3) contents actually match the chip (on-device hash via
        # esptool verify-flash, not just the file size).
        size = os.path.getsize(tmp)
        if size != FLASH_SIZE:
            print(f"ERROR: backup is {size} bytes, expected exactly {FLASH_SIZE}. NOT flashing.")
            return None, None
        with open(tmp, "rb") as f:
            magic = f.read(1)
        if magic != b"\xe9":
            print(f"ERROR: backup does not start with the ESP image magic (0xE9); got "
                  f"0x{magic.hex() or '??'}. The read looks blank/corrupt — NOT flashing.")
            return None, None
        print("      verifying backup against the chip (hash)...")
        # Start the verify at the rate that the read needed. The flash step
        # after the backup uses the rate that this function returns.
        ladder = [b for b in bauds if b <= baud]
        rc, _ = run_esptool(port, ladder, "verify-flash", "0x0", tmp)
        if rc != 0:
            print("ERROR: backup failed hash verification against the chip — NOT flashing.")
            return None, None
        os.replace(tmp, path)
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)

    digest = sha256_file(path)
    print(f"      backup OK: {size} bytes, verified.")
    print(f"      SHA-256: {digest}")
    print("      Keep this file safe — it is the ONLY way back to stock.")
    return path, baud


def do_flash(port, bauds, build_dir):
    args = []
    for offset, rel in IMAGES:
        p = os.path.join(build_dir, rel)
        if not os.path.exists(p):
            print(f"ERROR: missing build artifact {p}")
            print("       Build first:  ./tools/idf-build.sh . esp32c3 build")
            return 1
        args += [offset, p]
    app = os.path.join(build_dir, "dragonbreath.bin")
    print(f"\n[3/3] Flashing DragonBreath from {build_dir}")
    print(f"      app image SHA-256: {sha256_file(app)}")
    rc, _ = run_esptool(port, bauds,
                        "--before", "default-reset", "--after", "hard-reset",
                        "write-flash", "--flash-mode", "dio",
                        "--flash-size", "4MB", "--flash-freq", "80m", *args)
    return rc


def do_restore(port, bauds, image):
    if not os.path.exists(image):
        print(f"ERROR: backup image not found: {image}")
        return 1
    size = os.path.getsize(image)
    # A full-chip restore must be exactly the flash size — a wrong-size file would
    # write a truncated/misaligned image and brick the board.
    if size != FLASH_SIZE:
        print(f"ERROR: {image} is {size} bytes; a full restore must be exactly "
              f"{FLASH_SIZE} bytes (4 MB). Refusing.")
        return 1
    with open(image, "rb") as f:
        if f.read(1) != b"\xe9":
            print("ERROR: image does not start with the ESP magic (0xE9) — not a "
                  "valid full-flash image. Refusing.")
            return 1
    print(f"\nRestoring full flash image {image}")
    print(f"  size: {size} bytes   SHA-256: {sha256_file(image)}")
    if not confirm("This OVERWRITES the entire chip. Continue?"):
        print("Aborted.")
        return 1
    rc, _ = run_esptool(port, bauds,
                        "--before", "default-reset", "--after", "hard-reset",
                        "write-flash", "--flash-size", "detect", "0x0", image)
    if rc != 0:
        return rc
    # write-flash already verifies the write inline ("Hash of data verified") before
    # the reset. Do NOT run a second verify-flash here: --after hard-reset boots the
    # restored firmware, which immediately rewrites its own NVS and PHY/RF-calibration
    # regions, so a full-chip re-read no longer matches the file (guaranteed with an
    # nvs-scrubbed backup) and reports a *false* "digest mismatch."
    print("  restore complete — write hash-verified; the device is rebooting into "
          "the restored firmware.")
    return 0


def do_erase_nvs(port, bauds):
    # Erase ONLY the NVS/config region — the firmware is untouched. This is the
    # USB equivalent of the on-device Power+Auto reset combo: it clears Wi-Fi
    # credentials, the control token, saved policy, and sensor calibration, so
    # the device comes back up unconfigured (its own AP for re-provisioning).
    print(f"\nErase config only: NVS region {hex(NVS_OFFSET)} + {hex(NVS_SIZE)} "
          f"({NVS_SIZE // 1024} KB).")
    print("  Clears Wi-Fi credentials, control token, saved policy, and sensor")
    print("  calibration. The firmware is left intact; the device re-provisions")
    print("  (its own AP) on next boot. Same effect as the Power+Auto reset combo.")
    if not confirm("Erase the config (NVS)?"):
        print("Aborted.")
        return 1
    rc, _ = run_esptool(port, bauds, "erase-region", hex(NVS_OFFSET), hex(NVS_SIZE))
    if rc != 0:
        print("ERROR: NVS erase failed.")
        return rc
    print("  config erased — power-cycle the board; it will come up unconfigured.")
    return 0


def main():
    ap = argparse.ArgumentParser(description="DragonBreath flasher (backs up stock first).")
    ap.add_argument("--port", help="serial port (default: esptool auto-detect)")
    ap.add_argument("--baud", type=int, default=None,
                    help="pin a single baud rate; default auto-falls-back "
                         f"{'->'.join(str(b) for b in BAUD_LADDER)} on failure")
    ap.add_argument("--build-dir", default=os.path.join(REPO_ROOT, "build"),
                    help="ESP-IDF build dir (default: <repo>/build)")
    ap.add_argument("--backup-dir", default=os.path.join(REPO_ROOT, "backups"),
                    help="where to write the stock backup (default: <repo>/backups)")
    ap.add_argument("--restore", metavar="IMAGE",
                    help="restore a full flash image (e.g. a prior backup) and exit")
    ap.add_argument("--backup-only", action="store_true",
                    help="take a verified full stock backup and exit (no flashing) — "
                         "the recommended safety net before a no-USB OTA install")
    ap.add_argument("--no-backup", action="store_true",
                    help="skip the pre-flash backup (NOT recommended)")
    ap.add_argument("--erase-nvs", action="store_true",
                    help="erase ONLY the NVS/config region (Wi-Fi, control token, "
                         "policy, calibration) and exit — a config reset that keeps "
                         "the firmware; the USB twin of the Power+Auto reset combo")
    args = ap.parse_args()

    if not check_esptool():
        return 1

    # Pinned rate -> use only that; otherwise the fast->reliable auto-fallback ladder.
    bauds = [args.baud] if args.baud else list(BAUD_LADDER)

    if args.restore:
        return do_restore(args.port, bauds, args.restore)

    if args.erase_nvs:
        return do_erase_nvs(args.port, bauds)

    if args.backup_only:
        path, _ = do_backup(args.port, bauds, args.backup_dir)
        if path is None:
            return 1
        print(f"\nStock backup saved: {path}")
        print("Keep it safe — copy it off this machine (cloud/USB). Restore with:")
        print(f"  {script_command(sys.argv[0])} --restore {path}")
        return 0

    print("=" * 70)
    print(" DragonBreath flasher")
    print(" !! THIS OVERWRITES THE WHOLE DEVICE AND ERASES THE STOCK FIRMWARE !!")
    print(" There is NO way back to stock without a full backup, and BIGTREETECH")
    print(" does not publish stock images. The backup taken below is your ONLY")
    print(" recovery path — keep it safe and copy it off this machine.")
    print("=" * 70)
    if args.no_backup:
        print("\n*** --no-backup: SKIPPING the stock backup. ***")
        print("*** If you do not ALREADY have a known-good backup stored safely, you")
        print("*** will PERMANENTLY lose the ability to return to stock firmware. ***")
        if not confirm("Really flash WITHOUT taking a backup?"):
            print("Aborted."); return 1
        if not confirm("Are you SURE? This cannot be undone without a backup."):
            print("Aborted."); return 1
    else:
        path, used_baud = do_backup(args.port, bauds, args.backup_dir)
        if path is None:
            return 1
        # The backup returns the slowest rate that a chunk of its read needed.
        # Start the flash there (and below) so we don't re-hit a bad higher rate.
        bauds = [b for b in bauds if b <= used_baud] or [used_baud]
        print(f"\n*** IMPORTANT: keep {path} safe — it is the ONLY way back to")
        print("*** stock. Copy it off this machine (cloud/USB) before continuing. ***")

    print("\n[2/3] Ready to flash DragonBreath.")
    if not confirm("Proceed with flashing?"):
        print("Aborted. Your backup (if taken) is kept.")
        return 1

    rc = do_flash(args.port, bauds, args.build_dir)
    if rc == 0:
        print("\nDone. The board reboots into DragonBreath. On first boot with no saved")
        print("Wi-Fi it starts an 'DragonBreath_XXXX' AP — connect and open http://192.168.4.1")
    else:
        print("\nFlash FAILED. If the board is unresponsive, restore your backup:")
        print(f"  {script_command(sys.argv[0])} --restore <backup.bin>")
    return rc


if __name__ == "__main__":
    sys.exit(main())
