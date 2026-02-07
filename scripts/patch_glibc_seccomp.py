#!/usr/bin/env python3
"""
Patches glibc binaries (ld-linux-aarch64.so.1, libc.so.6) to avoid
Android seccomp kills on set_robust_list and rseq syscalls.

Android app processes inherit a strict seccomp filter from zygote that
blocks these syscalls with SECCOMP_RET_KILL_PROCESS (uncatchable).
glibc calls them during early dynamic linker init (before constructors),
so no signal handler can intercept them.

This script finds `svc #0` instructions preceded by `mov x8, #SYSCALL_NR`
and replaces the `svc #0` with `movn x0, #37` (= mov x0, #-38 = -ENOSYS).
glibc handles -ENOSYS gracefully for both syscalls.

Usage:
    python3 patch_glibc_seccomp.py <binary> [<binary2> ...]

Example:
    python3 patch_glibc_seccomp.py fex/lib/ld-linux-aarch64.so.1 fex/lib/libc.so.6
"""

import sys
import struct
import os
import shutil

# aarch64 syscall numbers to patch
BLOCKED_SYSCALLS = {
    99:  "set_robust_list",  # glibc __tls_init_tp → NPTL robust mutex
    293: "rseq",             # glibc __libc_early_init → restartable sequences
}

# aarch64 instruction encodings (little-endian)
SVC_0 = b'\x01\x00\x00\xd4'           # svc #0
MOVN_X0_37 = b'\xa0\x04\x80\x92'      # movn x0, #37  (x0 = -38 = -ENOSYS)


def encode_movz_x8(imm16):
    """Encode MOVZ X8, #imm16 as 4 bytes (little-endian)."""
    # MOVZ Xd: 1_10_100101_00_imm16_Rd
    # X8 = register 8, hw=0
    insn = (0b11010010100 << 21) | (imm16 << 5) | 8
    return struct.pack('<I', insn)


def patch_binary(filepath):
    """Patch a single binary file, replacing blocked syscall instructions."""
    with open(filepath, 'rb') as f:
        data = bytearray(f.read())

    original = bytes(data)
    total_patches = 0

    # Build lookup table: bytes for each blocked syscall's mov x8, #NR
    targets = {}
    for nr, name in BLOCKED_SYSCALLS.items():
        mov_bytes = encode_movz_x8(nr)
        targets[mov_bytes] = (nr, name)

    # Scan for svc #0 at 4-byte aligned offsets.
    # The compiler may place several instructions between mov x8,#NR and svc #0,
    # so we search up to LOOKBACK instructions before each svc.
    LOOKBACK = 16  # max instructions to look back for mov x8, #NR

    for offset in range(4, len(data) - 3, 4):
        if data[offset:offset+4] == SVC_0:
            # Search preceding instructions for mov x8, #blocked_nr
            for back in range(1, LOOKBACK + 1):
                prev_off = offset - back * 4
                if prev_off < 0:
                    break
                prev = bytes(data[prev_off:prev_off+4])
                if prev in targets:
                    nr, name = targets[prev]
                    print(f"  [0x{offset:08x}] Patching svc #0 (mov x8,#{nr} at 0x{prev_off:08x}, {back} insn before) ({name})")
                    data[offset:offset+4] = MOVN_X0_37
                    total_patches += 1
                    break
                # Stop if we hit another mov x8 (different syscall) or a branch
                insn = struct.unpack('<I', prev)[0]
                # Check for MOVZ X8 (any value): mask top 11 bits + Rd field
                if (insn & 0xFFE0001F) == 0xD2800008:
                    break  # different syscall number loaded into x8
                # Check for branch instructions (b, bl, ret, etc.)
                if (insn >> 26) in (0x05, 0x25, 0x6B):  # b, bl, ret/br
                    break

    if total_patches == 0:
        print(f"  No blocked syscalls found in {filepath}")
        return False

    # Write patched binary
    backup = filepath + '.orig'
    if not os.path.exists(backup):
        shutil.copy2(filepath, backup)
        print(f"  Backup saved: {backup}")

    with open(filepath, 'wb') as f:
        f.write(data)

    print(f"  Patched {total_patches} syscall(s) in {filepath}")
    return True


def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <binary> [<binary2> ...]")
        print(f"Example: {sys.argv[0]} fex/lib/ld-linux-aarch64.so.1 fex/lib/libc.so.6")
        sys.exit(1)

    any_patched = False
    for filepath in sys.argv[1:]:
        if not os.path.isfile(filepath):
            print(f"Error: {filepath} not found")
            continue
        print(f"Scanning {filepath}...")
        if patch_binary(filepath):
            any_patched = True

    if any_patched:
        print("\nDone! Blocked syscalls will now return -ENOSYS instead of killing the process.")
    else:
        print("\nNo patches applied. The binaries may already be patched or use different instruction sequences.")
        print("Check manually with: objdump -d <binary> | grep -B1 'svc.*#0x0'")


if __name__ == '__main__':
    main()
