"""Lift ArmyMen2.exe to C with pcrecomp, plus the jump-table arms its discovery misses.

`python -m tools` is pcrecomp's default pipeline; this is the same pipeline (its
translator's load_pe and generate_recomp_files) with one addition. MSVC's memcpy and
memmove dispatch through inline jump tables, `jmp dword ptr [reg*4 + table]`, and the
lifter turns each into a local switch over the labels it found in that function, falling
back to RECOMP_ITAIL for any other target. Discovery missed seven arms of memmove's
backward-copy tail (0x004659A4..0x004659D4), so the first long run ended in
"ITAIL: unresolved VA 0x004659AC" and a wild write two calls later.

So every `jmp [reg*4 + disp32]` in a function's bytes is found by its encoding
(FF 24, SIB with scale 4 and no base), its table is read while the entries stay inside
that function, and each arm that is neither a block of the function nor an entry already
gets a body of its own as an ALIAS: dispatchable, overlapping its host on purpose, never
a clamp limit (seeding it instead would make it a function start and cut the host short).

It also renames the functions the host wraps (WRAP below). The lifter turns `call X` into a plain
C call of sub_X, so the only way to put a host function in front of one is to rename the lifted
definition to sub_X_lifted and let the host (src/host/hires.c) define sub_X, which then runs first
and calls sub_X_lifted. Indirect calls (vtables) go through recomp_lookup_manual instead and need
no rename. The lifter also copies a function's body into a second function wherever another
function jumps into its middle ("split entries": the same code under another entry VA); those are
listed in recomp_wrap.h so the host can trap any that is ever reached, because a wrapper around the
main entry would not see them.

It also writes recomp_target.h, naming the exe the C was lifted from (MD5, size and,
when known, which release it is), so the host refuses to run lifted C against any other
build. Retail, GOG and the Russian edition share the PE timestamp and size, so only a
hash tells them apart, and the GOG exe differs from retail in code (six CD-check
branches): each player lifts the exe they own.

usage: python scripts/run_lift.py <ArmyMen2.exe> <out dir> [--report work/table_arms.json]
"""
import argparse
import hashlib
import json
import os
import re
import struct
import sys

# Known ArmyMen2.exe builds, by MD5. All are 585,728 B, PE timestamp
# 0x36B7AE36. Any other build lifts too; it is just reported as unknown.
KNOWN_BUILDS = {
    '59180d8113d1d756e838b0dab1790b6b': 'retail 1999, English CD',
    '78a6f5344559c2724f48862d22120b2f': '2017 re-release (GOG; Steam likely the same): CD check disabled by the publisher',
    '0c760f05a945d7276c67a753a838a295': 'Russian Edition (retail code, translated data)',
}


def drm_wrapped(exe_path):
    """The name of a DRM wrapper's section, or None.

    Steam's SteamStub adds a `.bind` section and encrypts .text behind it, so a lift
    would be of the wrapper, not the game. This project removes no copy protection
    (README, rule 3), so such an exe is refused rather than unwrapped, and pcrecomp's
    tools/drm/ scripts are never used.
    """
    import pefile
    pe = pefile.PE(exe_path, fast_load=True)
    for s in pe.sections:
        name = s.Name.rstrip(b'\0').decode('latin-1')
        if name == '.bind':
            return name
    return None

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.join(HERE, '..', 'third_party', 'pcrecomp', 'tools')
sys.path.insert(0, os.path.join(TOOLS, 'lift'))
import translator  # noqa: E402  (adds pe/, disasm/ and lift/ to sys.path itself)
from disasm32 import Disassembler  # noqa: E402

# SIB bytes for [index*4 + disp32] with no base register (mod=00, base=101), index != esp
TABLE_SIBS = {0x85, 0x8D, 0x95, 0x9D, 0xAD, 0xB5, 0xBD}


def read_table(image, base, table, step, lo, hi):
    """Entries inside [lo, hi) read from table in one direction (step +4 or -4)."""
    found, misses = [], 0
    for k in range(256):
        slot = table + step * k - base
        if slot < 0 or slot + 4 > len(image):
            break
        t = struct.unpack_from('<I', image, slot)[0]
        if lo <= t < hi:
            found.append(t)
            misses = 0
            continue
        # As disasm32's table_entries: the index can be biased, so tolerate a few
        # non-entries before the table starts and end it after two once it has.
        misses += 1
        if (found and misses >= 2) or (not found and misses >= 4):
            break
    return found


def table_arms(image, base, func):
    """Targets of every `jmp [reg*4 + table]` inside func's bytes that stay inside func.

    Tables are read both ways from the displacement: memmove's backward copy does
    `neg ecx; jmp [ecx*4 + 0x004659A0]`, so its index runs -7..0 and its table lies
    BELOW the displacement, where a forward read finds nothing.
    """
    lo, hi = func.address, func.end
    arms = set()
    for a in range(lo, hi - 6):
        o = a - base
        if image[o] != 0xFF or image[o + 1] != 0x24 or image[o + 2] not in TABLE_SIBS:
            continue
        table = struct.unpack_from('<I', image, o + 3)[0]
        for found in (read_table(image, base, table, 4, lo, hi),
                      read_table(image, base, table - 4, -4, lo, hi)):
            if len(found) >= 2:
                arms.update(found)
    return arms


# Functions the host wraps by name (src/host/hires.c): each definition is renamed sub_X_lifted.
#   0x0042C440 map load, 0x00413480 HUD create, 0x004143A0 HudPaint, 0x00412FE0 cursor draw,
#   0x00424CA0 edge scroll, 0x00424AD0 state change request, 0x004260C0 the state-2 (mission) frame's dispatch,
#   0x004609D0 / 0x00460CA0 / 0x00460FD0 the row pool A / row pool B / SEQ evictors (counted), 0x0041DCE0 the dirty
#   list's reset (its fill read), 0x0041E440 DrawMapObjects (the depth list's fill read after it)
#   0x0044D6D0 the dialogs' "capture the back buffer into the primary" (the host clears the dialog corner after it)
#   0x0042B5A0 the camera (the bottom HUD layout reports a view shorter by the console's height: its clamp and follow)
WRAP = (0x0042C440, 0x00413480, 0x004143A0, 0x00412FE0, 0x00424CA0, 0x00424AD0, 0x004260C0,
        0x004609D0, 0x00460CA0, 0x00460FD0, 0x0041DCE0, 0x0041E440, 0x0044D6D0, 0x0042B5A0)


def wrap_pass(out_dir, wrap=WRAP):
    """Rename the WRAP definitions to sub_X_lifted, declare them, and write recomp_wrap.h.

    Exactly one definition `void sub_X(void) {` must exist per VA, or this exits. Bodies are
    otherwise untouched. recomp_wrap.h lists the split entries of every wrapped function: the other
    functions whose entry address is an instruction address inside the wrapped body.
    """
    names = sorted(n for n in os.listdir(out_dir) if re.fullmatch(r'recomp_\d+\.c', n))
    texts = {}
    for n in names:
        with open(os.path.join(out_dir, n), 'r') as f:
            texts[n] = f.read()
    all_defs = {int(m.group(1), 16)
                for t in texts.values() for m in re.finditer(r'^void sub_([0-9A-F]{8})\(void\) \{', t, re.M)}
    # A wrapped function's entry instruction (`/* 0x<VA>: ...`) must appear once in all the lifted C: a second
    # copy means another function was lifted with a copy of this body's start, which the wrapper cannot see.
    for va in wrap:
        n_entry = sum(len(re.findall(r'/\* 0x%08X:' % va, t)) for t in texts.values())
        if n_entry != 1:
            sys.exit(f'wrap pass: the entry instruction of sub_{va:08X} appears {n_entry} times in the lifted C (expected 1)')
    splits = {}
    for va in wrap:
        pat = re.compile(r'^void sub_%08X\(void\) \{\n' % va, re.M)
        hits = [(n, m) for n, t in texts.items() for m in [pat.search(t)] if m]
        n_defs = sum(len(pat.findall(t)) for t in texts.values())
        if n_defs != 1:
            sys.exit(f'wrap pass: expected one definition of sub_{va:08X}, found {n_defs}')
        n, m = hits[0]
        t = texts[n]
        end = re.compile(r'^\}\n', re.M).search(t, m.end())
        body = t[m.end():end.start() if end else len(t)]
        inside = {int(a, 16) for a in re.findall(r'/\* 0x([0-9A-F]{8}):', body)}
        splits[va] = sorted(a for a in inside if a != va and a in all_defs)
        texts[n] = t[:m.start()] + 'void sub_%08X_lifted(void) {\n' % va + t[m.end():]
    for n, t in texts.items():
        with open(os.path.join(out_dir, n), 'w') as f:
            f.write(t)
    hdr = os.path.join(out_dir, 'recomp_funcs.h')
    with open(hdr, 'a') as f:
        f.write('\n/* scripts/run_lift.py wrap pass: the lifted bodies of functions the host wraps */\n')
        for va in wrap:
            f.write('void sub_%08X_lifted(void);  /* 0x%08X */\n' % (va, va))
    with open(os.path.join(out_dir, 'recomp_wrap.h'), 'w') as f:
        f.write('/* Auto-generated by scripts/run_lift.py - wrapped functions and their split entries. DO NOT EDIT */\n')
        f.write('#pragma once\n')
        for va in wrap:
            f.write('/* sub_%08X is wrapped: the host defines sub_%08X; the lifted body is sub_%08X_lifted */\n' % (va, va, va))
        f.write('static const uint32_t am2_split_entries[] = {\n')
        for va in wrap:
            for a in splits[va]:
                f.write('    0x%08Xu,  /* inside sub_%08X */\n' % (a, va))
        f.write('    0u\n};\n')
    print('[*] Wrapped (renamed to _lifted): ' + ', '.join(f'sub_{v:08X}' for v in wrap)
          + '; split entries: ' + ', '.join(f'sub_{a:08X}' for v in wrap for a in splits[v]))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('exe')
    ap.add_argument('out')
    ap.add_argument('--split', type=int, default=1000)
    ap.add_argument('--report', default=None, help='write the added arms as JSON')
    args = ap.parse_args()

    wrapper = drm_wrapped(args.exe)
    if wrapper:
        sys.exit(f"{args.exe} has a '{wrapper}' section: it is wrapped in Steam's DRM (SteamStub). "
                 "This project does not remove copy protection, so it cannot be lifted.")

    info, pe_data, iat_map = translator.load_pe(args.exe)
    functions = translator.discover_functions(pe_data, info, iat_map)
    disasm = Disassembler(pe_data, info.image_base, info.sections)
    # MSVC puts a jump table in .text beside its code, so the code range is enough.
    code = bytes(disasm.read_bytes(info.code_start, info.code_end - info.code_start) or b'')
    if len(code) < info.code_end - info.code_start:
        sys.exit('cannot read the code range through Disassembler.read_bytes')

    added = {}
    for addr, func in sorted(functions.items()):
        if func.entry_kind != 'start' or not func.end:
            continue
        for arm in sorted(table_arms(code, info.code_start, func)):
            if arm in func.blocks or arm in functions or arm in added:
                continue
            body = disasm.disassemble_function(arm, iat_map)
            if not (body and body.blocks):
                continue
            body.entry_kind = 'alias'
            if not body.end:
                body.end = max(b.end for b in body.blocks.values())
                body.size = body.end - arm
            added[arm] = (addr, body)
    for arm, (_, body) in added.items():
        functions[arm] = body
    hosts = sorted({host for host, _ in added.values()})
    print(f"[*] Jump-table arms added as aliases: {len(added)} in {len(hosts)} function(s): "
          + ', '.join(f'sub_{h:08X}' for h in hosts))

    translator.generate_recomp_files(functions, iat_map, args.out, args.split)
    wrap_pass(args.out)

    with open(args.exe, 'rb') as f:
        exe = f.read()
    md5 = hashlib.md5(exe).hexdigest()
    build = KNOWN_BUILDS.get(md5, 'unknown build')
    with open(os.path.join(args.out, 'recomp_target.h'), 'w') as f:
        f.write('/* Auto-generated by scripts/run_lift.py - the exe this C was lifted from. DO NOT EDIT */\n')
        f.write('#pragma once\n')
        f.write(f'#define AM2_LIFT_MD5   "{md5}"\n')
        f.write(f'#define AM2_LIFT_SIZE  {len(exe)}u\n')
        f.write(f'#define AM2_LIFT_BUILD "{build}"\n')
    print(f"[*] Lifted from {os.path.basename(args.exe)}: {build}, md5 {md5}")
    if args.report:
        with open(args.report, 'w') as f:
            json.dump({f'0x{arm:08X}': f'sub_{host:08X}' for arm, (host, _) in sorted(added.items())},
                      f, indent=1)


if __name__ == '__main__':
    main()
