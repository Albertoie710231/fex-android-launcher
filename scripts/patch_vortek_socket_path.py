#!/usr/bin/env python3
"""
Binary-patch libvulkan_vortek.so to replace hardcoded Winlator paths
with our app's paths.

Patches applied:
1. Socket path:
   Old: /data/data/com.winlator/files/rootfs/tmp/.vortek/V0 (51 chars)
   New: /data/data/com.mediatek.steamlauncher/V0             (40 chars)
   The V0 symlink in our app's data dir points to the actual socket.

2. RUNPATH (ELF library search path):
   Old: /data/data/com.winlator/files/rootfs/lib (42 chars)
   New: our app's fex/lib directory path
   This is where glibc's libc.so.6 and ld-linux-aarch64.so.1 are found.
"""

import sys
import os

# Patch 1: Socket path
OLD_SOCKET = b"/data/data/com.winlator/files/rootfs/tmp/.vortek/V0"
NEW_SOCKET = b"/data/data/com.mediatek.steamlauncher/V0"

# Patch 2: RUNPATH (library search path) — null it out so LD_LIBRARY_PATH is used
OLD_RUNPATH = b"/data/data/com.winlator/files/rootfs/lib"
NEW_RUNPATH = b""  # Empty = no RUNPATH, falls back to LD_LIBRARY_PATH

def patch_binary(data, old, new, name):
    count = data.count(old)
    if count == 0:
        print(f"  WARNING: {name} old path not found (already patched?)")
        return data

    if len(new) > len(old):
        print(f"  ERROR: {name} new path ({len(new)}) longer than old ({len(old)})")
        sys.exit(1)

    padded = new + b"\x00" * (len(old) - len(new))
    data = data.replace(old, padded)
    print(f"  {name}: {old.decode()} -> {new.decode()}")
    print(f"    ({count} occurrence(s), padded with {len(old) - len(new)} null bytes)")
    return data

def patch_file(filepath):
    with open(filepath, "rb") as f:
        data = f.read()

    print(f"Patching {filepath}:")
    data = patch_binary(data, OLD_SOCKET, NEW_SOCKET, "Socket path")
    data = patch_binary(data, OLD_RUNPATH, NEW_RUNPATH, "RUNPATH")

    with open(filepath, "wb") as f:
        f.write(data)

    print("Done.")

if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <libvulkan_vortek.so>")
        sys.exit(1)

    patch_file(sys.argv[1])
