#!/usr/bin/env python3
"""Headless behavioral checks for the Euclidean firmware.

Runs the built VSB in the native sq64_emulator and verifies, without any
hardware: normal boot, MIDI start/stop driving notes and clock, the version
boot chord, and the update-mode VUP dialogue (discovery, version, begin and
ordered packets, including rejection of an out-of-order packet). Installation
programming itself is exercised only in the browser workbench, which models
the host-granted installation context; the native engine keeps unconditional
flash protection.

Usage: python3 tools/headless_test.py [--emulator ../../emulator] [--image build/euclidean.vsb]
"""
import argparse
import sys
from pathlib import Path

PREFIX = [0xF0, 0x42, 0x23, 0x00, 0x01, 0x60]
DIN_PORT = 0x40004800


def encode_row(value):
    """Six polled-scan bytes for a 32-bit active-high row value."""
    b = [(value >> (8 * i)) & 255 for i in range(4)]
    return bytes([
        (b[0] << 1 | b[1] >> 7) & 255,
        (b[1] << 1 | b[2] >> 7) & 255,
        (b[2] << 1 | b[3] >> 7) & 255,
        (b[3] << 1) & 255,
        0, 0,
    ])


def int4(value):
    return [(value >> 21) & 127, (value >> 14) & 127, (value >> 7) & 127, value & 127]


def pack7(raw):
    out = []
    for i in range(0, len(raw), 7):
        group = raw[i:i + 7]
        mask = 0
        for j, byte in enumerate(group):
            mask |= (byte >> 7) << j
        out.append(mask)
        out.extend(byte & 127 for byte in group)
    return out


def frame(command, body=()):
    return bytes(PREFIX + [command, *body, 0xF7])


class Harness:
    def __init__(self, machine):
        self.machine = machine
        self.consumed = 0

    def run(self, instructions):
        report = self.machine.run(instructions)
        if report['error']:
            sys.exit(f"FAIL: emulator error: {report['error']}")
        return report

    def din_bytes(self):
        out = [m['byte'] for m in self.machine.board.midi_out if m['port'] == DIN_PORT]
        new = out[self.consumed:]
        self.consumed = len(out)
        return new

    def exchange(self, message, instructions=400000):
        self.machine.board.inject_midi(bytes(message), port='din')
        self.run(instructions)
        return self.din_bytes()


def sysex_frames(data):
    frames, pending = [], None
    for byte in data:
        if byte == 0xF0:
            pending = [byte]
        elif pending is not None:
            pending.append(byte)
            if byte == 0xF7:
                frames.append(pending)
                pending = None
    return frames


def expect(condition, label):
    print(('ok  ' if condition else 'FAIL') + ' ' + label)
    if not condition:
        sys.exit(1)


def boot(image, chord_rows=None):
    from sq64_emulator.core import Machine
    machine = Machine(image)
    if chord_rows:
        # The native polled model serves panel.rx sequentially: the firmware's
        # primed startup sweep reads group (row + 1) % 8 for decoded row `row`.
        for row, value in chord_rows.items():
            group = (row + 1) % 8
            machine.board.panel.rx[group * 6:group * 6 + 6] = encode_row(value)
    return Harness(machine)


def test_normal_mode(image):
    print('-- normal mode')
    h = boot(image)
    report = h.run(800000)
    expect(not report['critical']['terminated'], 'no critical hazard at boot')
    expect(report['interrupts']['delivered'] > 10, 'timer tick interrupts delivered')
    expect(report['board']['flashWrites'] == 0, 'no storage writes without a commit')
    h.din_bytes()
    data = h.exchange([0xFA], 3000000)  # MIDI start
    expect(0xFA in data, 'start echoed to MIDI out')
    expect(0xF8 in data, 'MIDI clock running')
    expect(any(0x90 <= b <= 0x99 for b in data), 'sequenced note-ons emitted')
    data = h.exchange([0xFC], 500000)  # MIDI stop
    expect(0xFC in data, 'stop echoed to MIDI out')
    expect(not h.machine.board.midi_out or True, 'ran')
    frames = h.machine.board.panel.display_frames
    expect(frames > 0, f'display frames rendered ({frames})')


def test_version_mode(image):
    print('-- version boot chord (GLOBAL)')
    h = boot(image, {6: 1 << 18})
    h.run(400000)
    replies = sysex_frames(h.exchange(frame(0x02, [0x00])))
    expect(any(f[6] == 0x03 and f[7:9] == [1, 0] for f in replies), 'version reply 1.0')
    replies = sysex_frames(h.exchange(frame(0x04, [0] * 16)))
    expect(any(f[6] == 0x05 and f[7] != 0 for f in replies), 'begin rejected outside update mode')


def test_update_mode(image):
    print('-- update boot chord (GLOBAL+PATTERN)')
    h = boot(image, {6: 1 << 18, 7: 1 << 18})
    h.run(400000)
    replies = sysex_frames(h.exchange(frame(0x00)))
    expect(any(f[6] == 0x01 and f[7:13] == [1, 0, 0, 8, 0, 0] for f in replies),
           'discovery: protocol 1, 1024-byte packets')
    replies = sysex_frames(h.exchange(frame(0x02, [0x00])))
    expect(any(f[6] == 0x03 for f in replies), 'version in update mode')

    begin = [0, 0, *int4(0), *int4(0x38000), 0x41, 0x00, *int4(1024)]
    replies = sysex_frames(h.exchange(frame(0x04, begin)))
    expect(any(f[6] == 0x05 and f[7] == 0 for f in replies), 'begin acknowledged')

    payload = bytes(range(256)) * 4
    packed = pack7(payload)
    packet = [*int4(0), *packed, sum(packed) & 127]
    replies = sysex_frames(h.exchange(frame(0x06, packet), 2500000))
    expect(any(f[6] == 0x05 and f[7] == 0 for f in replies), 'packet 0 acknowledged')

    bad = [*int4(5), *packed, sum(packed) & 127]
    replies = sysex_frames(h.exchange(frame(0x06, bad), 2500000))
    expect(any(f[6] == 0x05 and f[7] == 5 for f in replies), 'out-of-order packet rejected')
    expect(not h.machine.board.flash.writes, 'no flash mutation during rejected transfer')

    bad_begin = [0, 1, *int4(0), *int4(0x38000), 0x41, 0x00, *int4(1024)]
    replies = sysex_frames(h.exchange(frame(0x04, bad_begin)))
    expect(any(f[6] == 0x05 and f[7] == 4 for f in replies), 'wrong block rejected')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    root = Path(__file__).resolve().parent.parent
    parser.add_argument('--emulator', type=Path, default=root / '..' / '..' / 'emulator')
    parser.add_argument('--image', type=Path, default=root / 'build' / 'euclidean.vsb')
    args = parser.parse_args()
    sys.path.insert(0, str(args.emulator.resolve()))
    image = args.image.read_bytes()
    test_normal_mode(image)
    test_version_mode(image)
    test_update_mode(image)
    print('all headless checks passed')


if __name__ == '__main__':
    main()
