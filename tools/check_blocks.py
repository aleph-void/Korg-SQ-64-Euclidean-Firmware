#!/usr/bin/env python3
"""Verify no straight-line code run exceeds the browser engine's TB budget.

The emulator's patched TCI interpreter (unicorn-wasm *-tci-itstate) has a
fixed translation budget per basic block: around 120 simple instructions,
and fewer when the block is load/store heavy, because the per-instruction
observation ops share the same buffer. Overflowing it traps the WASM host
("memory access out of bounds") before the block runs a single instruction.

A translation block spans from its entry to the next instruction that writes
the PC, so the metric checked here is the longest run of consecutive
instructions without a control transfer. The limit of 80 keeps a comfortable
margin below the observed trap threshold for store-heavy code (a 95-
instruction spill run trapped; 83 ran).

Usage: check_blocks.py build/euclidean.elf [--objdump arm-none-eabi-objdump]
"""
import argparse
import re
import subprocess
import sys

LIMIT = 80

# Thumb instructions that end a translation block (any PC write).
BRANCH = re.compile(r'^(b|bl|blx|bx|beq|bne|bcs|bcc|bmi|bpl|bvs|bvc|bhi|bls'
                    r'|bge|blt|bgt|ble|svc|udf)(\.n|\.w)?$')


def functions(objdump, elf):
    """Function symbol intervals; data between them is never executed."""
    table = subprocess.run([objdump, '-t', elf],
                           capture_output=True, text=True, check=True).stdout
    spans = []
    for line in table.splitlines():
        m = re.match(r'^([0-9a-f]+) (.{7}) (\S+)\t([0-9a-f]+)', line)
        if m and 'F' in m.group(2):
            start, size = int(m.group(1), 16), int(m.group(4), 16)
            if size:
                spans.append((start & ~1, (start & ~1) + size))
    return spans


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('elf')
    parser.add_argument('--objdump', default='arm-none-eabi-objdump')
    args = parser.parse_args()

    spans = sorted(functions(args.objdump, args.elf))
    listing = subprocess.run([args.objdump, '-d', args.elf],
                             capture_output=True, text=True, check=True).stdout

    def in_function(address):
        for start, end in spans:
            if start <= address < end:
                return True
        return False

    run_start, run_length = None, 0
    worst = (0, None)
    for line in listing.splitlines():
        m = re.match(r'^\s+([0-9a-f]+):\s+[0-9a-f]+(?: [0-9a-f]+)?\s+(\S+)', line)
        if not m:
            continue
        address, mnemonic = m.group(1), m.group(2)
        ends = (BRANCH.match(mnemonic)
                or mnemonic in ('.word', '.short')     # literal pools
                or not in_function(int(address, 16))   # inter-function data
                or (mnemonic == 'pop' and 'pc' in line)
                or (mnemonic == 'mov' and re.search(r'\bpc,', line)))
        if ends:
            if run_length > worst[0]:
                worst = (run_length, run_start)
            run_start, run_length = None, 0
            continue
        if run_start is None:
            run_start = address
        run_length += 1
    if run_length > worst[0]:
        worst = (run_length, run_start)

    length, start = worst
    print(f'longest straight-line run: {length} instructions'
          + (f' (from 0x{start})' if start else ''))
    if length > LIMIT:
        print(f'FAIL: exceeds the {LIMIT}-instruction translation-block '
              'budget of the browser engine; split the block '
              '(e.g. NOINLINE in src/main.c)', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
