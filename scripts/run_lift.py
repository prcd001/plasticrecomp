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

usage: python scripts/run_lift.py <ArmyMen2.exe> <out dir> [--report work/table_arms.json]
"""
import argparse
import json
import os
import struct
import sys

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


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('exe')
    ap.add_argument('out')
    ap.add_argument('--split', type=int, default=1000)
    ap.add_argument('--report', default=None, help='write the added arms as JSON')
    args = ap.parse_args()

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
    if args.report:
        with open(args.report, 'w') as f:
            json.dump({f'0x{arm:08X}': f'sub_{host:08X}' for arm, (host, _) in sorted(added.items())},
                      f, indent=1)


if __name__ == '__main__':
    main()
