#!/usr/bin/env python3
"""Package a 64 KiB application binary into a full-system SQ-64 VSB.

The container format comes from the independent SQ-64 specification
(spec/docs/contracts.md): a 256-byte header followed by a 0x38000-byte
payload. Payload regions that this firmware does not use (the SPI-resident
regions) are filled with 0xFF, and each documented checksum region gets a
correction halfword so its little-endian halfword sum is zero modulo 65536.
No vendor data is embedded.
"""
import argparse
import struct
import sys
from pathlib import Path

PAYLOAD_SIZE = 0x38000
INTERNAL_SIZE = 0x10000
HEADER_SIZE = 0x100
# (offset, length, correction halfword offset) for each checksum region.
REGIONS = [
    (0x00000, 0x10000, 0x0fffe),
    (0x10000, 0x04000, 0x13ffe),
    (0x14000, 0x0bdd8, 0x1fdd6),
    (0x20000, 0x14000, 0x33ffe),
]


def build(binary: bytes, major: int, minor: int) -> bytes:
    if len(binary) > INTERNAL_SIZE - 2:
        sys.exit(f'Application binary too large: {len(binary)} > {INTERNAL_SIZE - 2}')
    if not 0 <= major < 64 or not 0 <= minor < 128:
        sys.exit('Version must satisfy major < 64 and minor < 128')
    payload = bytearray([0xff]) * PAYLOAD_SIZE
    payload[:len(binary)] = binary

    for offset, length, correction in REGIONS:
        if payload[correction:correction + 2] != b'\xff\xff':
            sys.exit(f'Checksum correction slot 0x{correction:x} is occupied')
        total = sum(struct.unpack_from(f'<{length // 2}H', payload, offset)) & 0xffff
        old = struct.unpack_from('<H', payload, correction)[0]
        struct.pack_into('<H', payload, correction, (old - total) & 0xffff)

    header = bytearray(HEADER_SIZE)
    header[0x00:0x10] = b'KORG SYSTEM FILE'
    header[0x10:0x20] = b'SQ-64'.ljust(16, b'\0')
    header[0x20:0x28] = b'SYSTEM\0\0'
    header[0x29] = 0  # block
    header[0x2a] = major
    header[0x2b] = minor
    header[0x2c:0x30] = bytes([0x00, 0x01, 0x60, 0xff])
    struct.pack_into('<4I', header, 0x30, 0, PAYLOAD_SIZE, 0, PAYLOAD_SIZE)
    struct.pack_into('<I', header, 0x44, 0xffffffff)  # no post-update delay
    return bytes(header) + bytes(payload)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--major', type=int, default=1)
    parser.add_argument('--minor', type=int, default=0)
    args = parser.parse_args()
    package = build(args.binary.read_bytes(), args.major, args.minor)
    args.output.write_bytes(package)
    print(f'{args.output} ({len(package)} bytes, version {args.major}.{args.minor})')


if __name__ == '__main__':
    main()
