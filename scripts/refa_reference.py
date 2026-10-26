"""Turn Reference A's docs/functions.tsv into a score_recovery reference catalog.

Reference A (borancar/ArmyMen2) lists every function start in ArmyMen2.exe with its
size, covering 100% of .text, so each size runs into the next function's padding.
score_recovery wants ends without that padding, so each end is walked back over
trailing 0xCC / 0x90 bytes, read from the exe itself.

usage: python scripts/refa_reference.py <functions.tsv> <ArmyMen2.exe> <out.json>
"""
import csv
import json
import sys

import pefile


def main(tsv_path, exe_path, out_path):
    pe = pefile.PE(exe_path, fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    image = pe.get_memory_mapped_image()

    functions = []
    with open(tsv_path, newline='') as f:
        for row in csv.DictReader(f, delimiter='\t'):
            start = int(row['addr'], 16)
            end = start + int(row['size'])
            while end > start + 1 and image[end - 1 - base] in (0xCC, 0x90):
                end -= 1
            functions.append({'address': start, 'end': end})

    with open(out_path, 'w') as f:
        json.dump({'source': 'ArmyMen2-master/docs/functions.tsv',
                   'functions': functions}, f)
    print(f'{len(functions)} reference functions -> {out_path}')


if __name__ == '__main__':
    main(*sys.argv[1:4])
