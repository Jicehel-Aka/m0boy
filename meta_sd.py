#!/usr/bin/env python3
"""Agent-friendly operations on the mounted META SD mass-storage volume."""
import argparse
import ctypes
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import stat
import sys
import tempfile
import zipfile


def within(root: Path, name: str) -> Path:
    if not name or "\\" in name or "\0" in name:
        raise ValueError("invalid SD path")
    p = PurePosixPath(name)
    if p.is_absolute() or any(x in ("", ".", "..") for x in p.parts):
        raise ValueError(f"path escapes SD root: {name}")
    result = root
    for part in p.parts:
        result = result / part
        if result.is_symlink():
            raise ValueError(f"symlink inside SD root: {result}")
    return result


def file_copy(source, dest: Path) -> int:
    dest.parent.mkdir(parents=True, exist_ok=True)
    if dest.exists() and dest.is_dir():
        raise IsADirectoryError(dest)
    fd, temp_name = tempfile.mkstemp(prefix=".m0transfer-", dir=dest.parent)
    try:
        with os.fdopen(fd, "wb") as out:
            total = shutil.copyfileobj(source, out, 64 * 1024)
            out.flush()
            os.fsync(out.fileno())
        os.replace(temp_name, dest)
    finally:
        if os.path.exists(temp_name):
            os.unlink(temp_name)
    return dest.stat().st_size


def unzip(root: Path, archive: Path, destination: str) -> dict:
    base = within(root, destination)
    count = size = 0
    with zipfile.ZipFile(archive) as zf:
        members = zf.infolist()
        # Validate every entry before changing the card.
        for info in members:
            if info.filename.startswith("/") or "\\" in info.filename:
                raise ValueError(f"unsafe ZIP member: {info.filename}")
            mode = info.external_attr >> 16
            if stat.S_ISLNK(mode):
                raise ValueError(f"ZIP symlink refused: {info.filename}")
            within(root, str(PurePosixPath(destination) / info.filename.rstrip("/")))
            if info.file_size > 4 * 1024 * 1024 * 1024:
                raise ValueError("ZIP member exceeds FAT32 file limit")
        for info in members:
            target = within(root, str(PurePosixPath(destination) / info.filename.rstrip("/")))
            if info.is_dir():
                target.mkdir(parents=True, exist_ok=True)
                continue
            with zf.open(info) as source:
                size += file_copy(source, target)
            count += 1
    return {"files": count, "bytes": size, "destination": str(base)}


def bootloader() -> dict:
    """Send the bridge's explicit vendor request (requires USB write access)."""
    usb = ctypes.CDLL("libusb-1.0.so.0")
    usb.libusb_init.argtypes = [ctypes.POINTER(ctypes.c_void_p)]
    usb.libusb_open_device_with_vid_pid.argtypes = [ctypes.c_void_p, ctypes.c_uint16, ctypes.c_uint16]
    usb.libusb_open_device_with_vid_pid.restype = ctypes.c_void_p
    usb.libusb_control_transfer.argtypes = [ctypes.c_void_p, ctypes.c_uint8,
                                             ctypes.c_uint8, ctypes.c_uint16,
                                             ctypes.c_uint16, ctypes.c_void_p,
                                             ctypes.c_uint16, ctypes.c_uint]
    usb.libusb_close.argtypes = [ctypes.c_void_p]
    usb.libusb_exit.argtypes = [ctypes.c_void_p]
    context = ctypes.c_void_p()
    if usb.libusb_init(ctypes.byref(context)) != 0:
        raise OSError("libusb initialization failed")
    try:
        handle = usb.libusb_open_device_with_vid_pid(context, 0x2341, 0x804d)
        if not handle:
            raise OSError("META SD Bridge inaccessible; check USB permissions")
        try:
            result = usb.libusb_control_transfer(handle, 0x40, 0x5a, 0x424c, 0,
                                                  None, 0, 1000)
            if result != 0:
                raise OSError(f"bootloader USB request failed ({result})")
        finally:
            usb.libusb_close(handle)
    finally:
        usb.libusb_exit(context)
    return {"bootloader": True}


def leave_usb_mode() -> dict:
    """Unmount the bridge and eject it: the firmware takes that SCSI eject
    (START STOP UNIT, LOEJ) as the way out and restarts into the Gamebuino
    loader.  No USB privileges needed (udisks); HOME+MENU held 0.5 s on the
    console does the same."""
    import subprocess
    disks = [line.split(None, 1) for line in subprocess.run(
        ["lsblk", "-dno", "NAME,MODEL"], capture_output=True, text=True, check=True).stdout.splitlines()]
    names = [f"/dev/{d[0]}" for d in disks if len(d) == 2 and "META SD BRIDGE" in d[1].upper().replace("_", " ")]
    if not names:
        raise OSError("META SD Bridge not found (already left USB mode?)")
    disk = names[0]
    parts = subprocess.run(["lsblk", "-lnpo", "NAME,MOUNTPOINT", disk], capture_output=True, text=True).stdout.splitlines()
    for line in parts:
        fields = line.split(None, 1)
        if len(fields) == 2:
            subprocess.run(["sync"], check=False)
            done = subprocess.run(["udisksctl", "unmount", "-b", fields[0]], capture_output=True, text=True)
            if done.returncode != 0:  # a file manager window keeps it busy: the data is synced, force it
                subprocess.run(["udisksctl", "unmount", "-f", "-b", fields[0]], capture_output=True, text=True)
    import time
    for _ in range(8):  # udisks answers "busy" for a few seconds after an unmount
        done = subprocess.run(["udisksctl", "power-off", "-b", disk], capture_output=True, text=True)
        if done.returncode == 0:
            break
        time.sleep(3)
    else:
        raise OSError("eject refused: " + (done.stderr or done.stdout).strip())
    return {"exit": True, "device": disk}


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", type=Path, default=os.environ.get("META_SD_MOUNT"),
                    help="mount point of the META SD card (or META_SD_MOUNT)")
    sub = ap.add_subparsers(dest="command", required=True)
    ls = sub.add_parser("ls"); ls.add_argument("path", nargs="?", default=".")
    put = sub.add_parser("put"); put.add_argument("source", type=Path)
    put.add_argument("destination", nargs="?", default=".")
    put.add_argument("--raw", action="store_true", help="copy a ZIP without extracting")
    get = sub.add_parser("get"); get.add_argument("source"); get.add_argument("destination", type=Path)
    mkdir = sub.add_parser("mkdir"); mkdir.add_argument("path")
    rm = sub.add_parser("rm"); rm.add_argument("path")
    uz = sub.add_parser("unzip"); uz.add_argument("archive", type=Path)
    uz.add_argument("destination", nargs="?", default=".")
    sub.add_parser("bootloader")
    sub.add_parser("exit", help="unmount and eject: the console leaves USB mode for the Gamebuino loader")
    args = ap.parse_args()
    if args.command == "exit":
        print(json.dumps({"ok": True, **leave_usb_mode()}))
        return
    if args.command == "bootloader":
        print(json.dumps({"ok": True, **bootloader()}))
        return
    if args.root is None:
        raise ValueError("--root or META_SD_MOUNT is required")
    root = args.root.resolve(strict=True)
    if not root.is_dir() or not os.path.ismount(root):
        raise ValueError(f"not a mounted volume: {root}")
    if args.command == "ls":
        p = within(root, args.path)
        entries = sorted(p.iterdir(), key=lambda x: x.name.lower()) if p.is_dir() else [p]
        result = [{"name": x.name, "directory": x.is_dir(),
                   "bytes": x.stat().st_size if x.is_file() else None} for x in entries]
    elif args.command == "put":
        if args.source.is_dir():
            base = within(root, args.destination)
            count = total = 0
            for source in args.source.rglob("*"):
                rel = source.relative_to(args.source).as_posix()
                target = within(root, str(PurePosixPath(args.destination) / rel))
                if source.is_symlink():
                    raise ValueError(f"source symlink refused: {source}")
                if source.is_dir(): target.mkdir(parents=True, exist_ok=True)
                else:
                    with source.open("rb") as inp: total += file_copy(inp, target)
                    count += 1
            result = {"files": count, "bytes": total}
        elif args.source.suffix.lower() == ".zip" and not args.raw:
            result = unzip(root, args.source, args.destination)
        else:
            dst = within(root, args.destination)
            if dst.is_dir(): dst = within(root, str(PurePosixPath(args.destination) / args.source.name))
            with args.source.open("rb") as inp: n = file_copy(inp, dst)
            result = {"files": 1, "bytes": n, "destination": str(dst)}
    elif args.command == "get":
        source = within(root, args.source)
        if source.is_dir():
            count = total = 0
            for item in source.rglob("*"):
                rel = item.relative_to(source)
                safe = within(root, str(PurePosixPath(args.source) / rel.as_posix()))
                dest = args.destination / rel
                if safe.is_dir(): dest.mkdir(parents=True, exist_ok=True)
                else:
                    with safe.open("rb") as inp: total += file_copy(inp, dest)
                    count += 1
            result = {"files": count, "bytes": total, "destination": str(args.destination)}
        else:
            args.destination.parent.mkdir(parents=True, exist_ok=True)
            with source.open("rb") as inp: n = file_copy(inp, args.destination)
            result = {"bytes": n, "destination": str(args.destination)}
    elif args.command == "mkdir":
        p = within(root, args.path); p.mkdir(parents=True, exist_ok=True)
        result = {"path": str(p)}
    elif args.command == "rm":
        p = within(root, args.path)
        if p.is_dir(): shutil.rmtree(p)
        else: p.unlink()
        result = {"removed": str(p)}
    else:
        result = unzip(root, args.archive, args.destination)
    print(json.dumps({"ok": True, **({"entries": result} if isinstance(result, list) else result)}, ensure_ascii=False))


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, NotImplementedError, zipfile.BadZipFile) as exc:
        print(json.dumps({"ok": False, "error": str(exc)}, ensure_ascii=False), file=sys.stderr)
        sys.exit(1)
