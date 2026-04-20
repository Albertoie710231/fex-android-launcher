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
                # BLR Rt = 0xD63F0000 | Rt<<5; BR Rt (tail-call) = 0xD61F0000 | Rt<<5
                if ins3 == (0xD63F0000 | (rt << 5)) or ins3 == (0xD61F0000 | (rt << 5)):
                    call_off = foff + 4 * step + 4 * step2
                    # BLR -> NOP. BR (tail) -> RET so caller returns normally.
                    repl = 0xD503201F if ins3 == (0xD63F0000 | (rt << 5)) else 0xD65F03C0
                    struct.pack_into('<I', data, call_off, repl)
                    hits += 1
                    # If a BRK #1 (poison) immediately follows a BLR _assert,
                    # NOP it too — execution resumes past the "unreachable" mark.
                    nxt = struct.unpack_from('<I', data, call_off + 4)[0]
                    if ins3 == (0xD63F0000 | (rt << 5)) and nxt == 0xD4200020:
                        struct.pack_into('<I', data, call_off + 4, 0xD503201F)
                    break
                # Bail if rt is overwritten before the call. ADRP/ADR/LDR/MOV
                # etc. all write Rd at bits 4-0; that's a conservative test
                # for most register-clobbering instructions in this region.
                if (ins3 & 0x1f) == rt:
                    break
            break

    # Pass 2: pointer-table indirect _assert calls. Pepelespooder's winevulkan
    # puts a pointer to a wrapper that calls _assert into a pointer table
    # (typically around .rdata/.data). Callers load from the pointer table,
    # then BLR into the wrapper; the wrapper then tail-calls _assert. The
    # caller sequence we want to disarm:
    #   adrp xR, <ptr_table_page>
    #   ldr  xR, [xR, #off]         ; loads wrapper pointer
    #   blr  xR
    #   brk  #1
    # We detect by finding BLR/BRK pairs whose preceding load reads from a
    # location whose STATIC file content is a pointer into .text. The file
    # contents of the IAT at rest are import-name-hint RVAs, so they match
    # the <= 0x80000000 range too; we additionally require the load address
    # to be OUTSIDE the IAT range (distinct from pass 1 above).
    txt_start_runtime = image_base + txt_vaddr
    txt_end_runtime = txt_start_runtime + txt_vsize

    def read_u64(rva):
        off = rva_to_off(rva)
        if off is None: return None
        return struct.unpack_from('<Q', data, off)[0]

    # Scan BLR followed by BRK in .text
    for off in range(0, txt_vsize - 8, 4):
        foff = txt_raddr + off
        blr = struct.unpack_from('<I', data, foff)[0]
        if (blr & 0xFFFFFC1F) != 0xD63F0000: continue  # not BLR xR
        brk = struct.unpack_from('<I', data, foff + 4)[0]
        if brk != 0xD4200020: continue  # not BRK #1
        rn = (blr >> 5) & 0x1f
        # Walk back looking for adrp+ldr that set xRn
        for back in range(1, 10):
            loff = foff - 4 * back
            if loff < txt_raddr: break
            ldr_ins = struct.unpack_from('<I', data, loff)[0]
            if ((ldr_ins >> 22) & 0x3ff) != 0x3e5: continue
            imm12 = (ldr_ins >> 10) & 0xfff
            ldr_rn = (ldr_ins >> 5) & 0x1f
            ldr_rt = ldr_ins & 0x1f
            if ldr_rt != rn: continue
            # Find adrp before LDR setting ldr_rn
            for back2 in range(1, 5):
                aoff = loff - 4 * back2
                if aoff < txt_raddr: break
                adrp_ins = struct.unpack_from('<I', data, aoff)[0]
                if ((adrp_ins >> 24) & 0x9F) != 0x90: continue
                adrp_rd = adrp_ins & 0x1f
                if adrp_rd != ldr_rn: continue
                immlo = (adrp_ins >> 29) & 3
                immhi = (adrp_ins >> 5) & 0x7ffff
                imm = (immhi << 2) | immlo
                if imm & 0x100000: imm |= ~((1 << 21) - 1)
                apc = image_base + txt_vaddr + (aoff - txt_raddr)
                target_page = (apc & ~0xfff) + (imm << 12)
                # Skip IAT pass-1 already handled
                if target_page == iat_page and (imm12 << 3) == iat_off_in_page: break
                # Read pointer at target_page+off
                ptr_rva = (target_page - image_base) + (imm12 << 3)
                ptr_val = read_u64(ptr_rva)
                if ptr_val is None: break
                if not (txt_start_runtime <= ptr_val < txt_end_runtime): break
                # Inspect the code at ptr_val — does it look like an _assert thunk?
                # (contains adrp to iat_page + ldr from iat_off)
                code_off = rva_to_off(ptr_val - image_base)
                if code_off is None: break
                found_assert = False
                for k in range(0, 24, 4):
                    if code_off + k + 4 > len(data): break
                    ci = struct.unpack_from('<I', data, code_off + k)[0]
                    if ((ci >> 24) & 0x9F) != 0x90: continue
                    ci_rd = ci & 0x1f
                    ci_immlo = (ci >> 29) & 3
                    ci_immhi = (ci >> 5) & 0x7ffff
                    ci_imm = (ci_immhi << 2) | ci_immlo
                    if ci_imm & 0x100000: ci_imm |= ~((1 << 21) - 1)
                    ci_pc = ptr_val + k
                    ci_target = (ci_pc & ~0xfff) + (ci_imm << 12)
                    if ci_target != iat_page: continue
                    # Check next insn for LDR from iat_off
                    if code_off + k + 4 + 4 > len(data): continue
                    ni = struct.unpack_from('<I', data, code_off + k + 4)[0]
                    if ((ni >> 22) & 0x3ff) != 0x3e5: continue
                    ni_imm12 = (ni >> 10) & 0xfff
                    ni_rn = (ni >> 5) & 0x1f
                    if ni_rn == ci_rd and (ni_imm12 << 3) == iat_off_in_page:
                        found_assert = True
                        break
                if found_assert:
                    struct.pack_into('<I', data, foff, 0xD503201F)  # NOP BLR
                    struct.pack_into('<I', data, foff + 4, 0xD503201F)  # NOP BRK
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
