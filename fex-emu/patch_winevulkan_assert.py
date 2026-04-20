#!/usr/bin/env python3
"""
NOP-out _assert() calls in Pepelespooder wine 10's aarch64 winevulkan.dll.

Pepelespooder compiled winevulkan.dll with asserts enabled. Its UNIX_CALL
thunk for vkCreateDevice runs `assert(!status)` after the call returns; on
ARM64EC the NTSTATUS is non-zero despite vkCreateDevice returning VK_SUCCESS,
which aborts the process. GameNative's proton-9 DLL was built with NDEBUG
(no _wassert/_assert imports), which is why GameNative runs past the same
point.

This script finds every `adrp x_, IAT_page; ldr x_, [x_, #off]; blr x_`
sequence in .text that calls ucrtbase's _assert, and replaces the final
BLR with an ARM64 NOP (0xD503201F). adrp+ldr are left as dead code; the
call itself becomes a no-op and wine continues past the assertion.

Tested on: files/proton11/lib/wine/aarch64-windows/winevulkan.dll from
Pepelespooder's wine 10.0.99-arm64ec drop (2740224 bytes, PE machine 0xaa64).

Usage: patch_winevulkan_assert.py INPUT.dll OUTPUT.dll
"""
import struct
import sys


def patch(in_path, out_path):
    with open(in_path, 'rb') as f:
        data = bytearray(f.read())

    pe_off = struct.unpack_from('<I', data, 0x3c)[0]
    assert data[pe_off:pe_off + 4] == b'PE\x00\x00'
    coff = pe_off + 4
    _, nsec, _, _, _, opthdr_size = struct.unpack_from('<HHIIIH', data, coff)
    opt_off = coff + 20
    magic = struct.unpack_from('<H', data, opt_off)[0]
    assert magic == 0x20b, f'expected PE32+, got 0x{magic:x}'
    image_base = struct.unpack_from('<Q', data, opt_off + 24)[0]
    import_dir_rva = struct.unpack_from('<I', data, opt_off + 112 + 8)[0]

    sec_off = opt_off + opthdr_size
    sections = []
    for i in range(nsec):
        s = sec_off + 40 * i
        name = data[s:s + 8].rstrip(b'\x00').decode('latin1')
        vsize, vaddr, _, raddr, _ = struct.unpack_from('<IIIII', data, s + 8)
        sections.append((name, vaddr, vsize, raddr))

    def rva_to_off(rva):
        for n, va, vs, ra in sections:
            if va <= rva < va + vs:
                return ra + (rva - va)
        return None

    # Find _assert IAT entry from ucrtbase imports.
    idesc_off = rva_to_off(import_dir_rva)
    iat_assert_rva = None
    i = 0
    while True:
        ilt_rva, _, _, name_rva, iat_rva_i = struct.unpack_from(
            '<IIIII', data, idesc_off + 20 * i)
        if ilt_rva == 0 and name_rva == 0:
            break
        dll_name = data[rva_to_off(name_rva):].split(b'\x00', 1)[0].decode('latin1')
        if dll_name.lower().startswith(('ucrt', 'msvcrt', 'api-ms-win-crt')):
            lt = rva_to_off(ilt_rva or iat_rva_i)
            k = 0
            while True:
                v = struct.unpack_from('<Q', data, lt + 8 * k)[0]
                if v == 0:
                    break
                if not (v & 0x8000000000000000):
                    hint_off = rva_to_off(v & 0x7fffffff)
                    fn_name = data[hint_off + 2:].split(b'\x00', 1)[0].decode('latin1')
                    if fn_name == '_assert':
                        iat_assert_rva = iat_rva_i + 8 * k
                k += 1
        i += 1
    if iat_assert_rva is None:
        sys.exit('could not locate _assert IAT entry')

    iat_entry_runtime = image_base + iat_assert_rva
    iat_page = iat_entry_runtime & ~0xfff
    iat_off_in_page = iat_entry_runtime - iat_page
    print(f'_assert IAT entry: runtime 0x{iat_entry_runtime:x}')

    # Scan .text for adrp (target=iat_page) + ldr (rn=adrp_rd, off=iat_off) +
    # blr (rn=ldr_rt). NOP the final BLR of every match.
    txt = next(s for s in sections if s[0] == '.text')
    _, txt_vaddr, txt_vsize, txt_raddr = txt

    hits = 0
    for off in range(0, txt_vsize - 12, 4):
        foff = txt_raddr + off
        pc = image_base + txt_vaddr + off
        ins1 = struct.unpack_from('<I', data, foff)[0]
        if (ins1 >> 24) & 0x9F != 0x90:
            continue  # not adrp
        immlo = (ins1 >> 29) & 3
        immhi = (ins1 >> 5) & 0x7ffff
        imm = (immhi << 2) | immlo
        if imm & 0x100000:
            imm |= ~((1 << 21) - 1)
        rd = ins1 & 0x1f
        if (pc & ~0xfff) + (imm << 12) != iat_page:
            continue
        for step in range(1, 6):
            ins2 = struct.unpack_from('<I', data, foff + 4 * step)[0]
            if (ins2 >> 22) & 0x3ff != 0x3e5:
                continue  # not LDR (immediate, unsigned offset, 64-bit)
            imm12 = (ins2 >> 10) & 0xfff
            rn = (ins2 >> 5) & 0x1f
            rt = ins2 & 0x1f
            if rn != rd or (imm12 << 3) != iat_off_in_page:
                break
            for step2 in range(1, 10):
                ins3 = struct.unpack_from('<I', data, foff + 4 * step + 4 * step2)[0]
                if ins3 == (0xD63F0000 | (rt << 5)):
                    blr_off = foff + 4 * step + 4 * step2
                    struct.pack_into('<I', data, blr_off, 0xD503201F)  # NOP
                    hits += 1
                    break
            break

    with open(out_path, 'wb') as f:
        f.write(data)
    print(f'patched {hits} BLR(s) -> NOP, wrote {out_path}')


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit(__doc__.strip().split('\n\n')[-1])
    patch(sys.argv[1], sys.argv[2])
