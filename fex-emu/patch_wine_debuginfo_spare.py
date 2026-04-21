#!/usr/bin/env python3
"""
NOP-out the `DebugInfo->Spare[0] = __FILE__ ":cs"` write in wine-built ARM64
PEs (services.exe, explorer.exe, ...).

Background: wine 10+ defaults InitializeCriticalSection to NOT allocate
RTL_CRITICAL_SECTION_DEBUG; it sets DebugInfo = (void *)-1 as a sentinel.
Pre-fix wine source (pre commit 95b0e65b, March 2024) calls plain
InitializeCriticalSection(&cs); immediately followed by
cs.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": ...");
which dereferences -1 and page-faults.

The fix upstream was to switch the callers to InitializeCriticalSectionEx
with RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO. Our Pepelespooder/proton
tree ships services.exe + explorer.exe built from the pre-fix source.
Rebuilding is impractical; instead we NOP the offending store.

The compiler-emitted sequence is invariably:

    adrp x8, <IAT page for InitializeCriticalSection>
    ldr  x8, [x8, #off]                ; x8 = ICS
    blr  x8                            ; InitializeCriticalSection(cs)
    ...
    adrp xN, <global holding cs struct>
    ldr  x7/xN, [xN, #off]             ; load struct pointer
    ldr  x8, [x7, #off]                ; x8 = cs.DebugInfo (offset of CS within struct)
    ...
    adrp x9, <__FILE__ ":name" string>
    add  x9, x9, #off
    str  x9, [x8, #0x28]               ; DebugInfo->Spare[0] = string ← FAULT

We NOP only the final store (encoding 0xF9001509 = `str x9, [x8, #0x28]`).
Losing the debug-name is harmless: it is only consumed by !crt.cs
diagnostics in winedbg, never by functional code.

Usage: patch_wine_debuginfo_spare.py INPUT.exe OUTPUT.exe
"""
import struct
import sys


NOP = bytes.fromhex('1f2003d5')  # little-endian encoding of `nop`

# `str <Rt>, [x8, #0x28]` — 64-bit store, base x8, unsigned offset 40.
# Encoded as 0xF9001500 | Rt where Rt is 0-31.
# We match any Rt since the compiler emits both `str x9, ...` (for writing
# a __FILE__ string pointer) and `str xzr, ...` (for writing 0 in the
# cleanup path — see wine services.c line 652).
STORE_BASE = 0xF9001500
STORE_MASK = 0xFFFFFFE0


def patch(in_path, out_path):
    import struct
    with open(in_path, 'rb') as f:
        data = bytearray(f.read())

    pe_off = struct.unpack_from('<I', data, 0x3c)[0]
    if data[pe_off:pe_off + 4] != b'PE\x00\x00':
        sys.exit(f'{in_path}: not a PE file')
    coff = pe_off + 4
    _, nsec, _, _, _, opthdr_size = struct.unpack_from('<HHIIIH', data, coff)
    opt_off = coff + 20
    magic = struct.unpack_from('<H', data, opt_off)[0]
    if magic != 0x20b:
        sys.exit(f'{in_path}: expected PE32+, got 0x{magic:x}')

    sec_off = opt_off + opthdr_size
    sections = []
    for i in range(nsec):
        s = sec_off + 40 * i
        name = data[s:s + 8].rstrip(b'\x00').decode('latin1')
        vsize, vaddr, rsize, raddr, _ = struct.unpack_from('<IIIII', data, s + 8)
        sections.append((name, vaddr, vsize, raddr, rsize))

    text = next((s for s in sections if s[0] == '.text'), None)
    if text is None:
        sys.exit(f'{in_path}: no .text section')
    _, tvaddr, tvsize, traddr, trsize = text
    tend = traddr + min(tvsize, trsize)

    # Scan .text 4 bytes at a time (ARM64 is fixed-width), matching any
    # `str <Rt>, [x8, #0x28]` via the mask.
    hits = []
    for off in range(traddr, tend, 4):
        w = struct.unpack_from('<I', data, off)[0]
        if (w & STORE_MASK) == STORE_BASE:
            hits.append(off)

    if not hits:
        sys.exit(f'{in_path}: no `str <Rt>, [x8, #0x28]` in .text; '
                 f'is the binary already patched or built from post-fix source?')

    for off in hits:
        rva = tvaddr + (off - traddr)
        old = struct.unpack_from('<I', data, off)[0]
        rt = old & 0x1f
        rt_name = 'xzr' if rt == 31 else f'x{rt}'
        data[off:off + 4] = NOP
        print(f'  NOPed str {rt_name},[x8,#0x28] at file 0x{off:x} (RVA 0x{rva:x})')

    with open(out_path, 'wb') as f:
        f.write(data)
    print(f'wrote {out_path} ({len(data)} bytes, {len(hits)} patch site(s))')


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit('usage: patch_wine_debuginfo_spare.py INPUT.exe OUTPUT.exe')
    patch(sys.argv[1], sys.argv[2])
