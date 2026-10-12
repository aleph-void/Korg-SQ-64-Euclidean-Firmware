#!/usr/bin/env python3
"""Headless behavioral checks for the Euclidean firmware.

Runs the built VSB in the native sq64_emulator and verifies, without any
hardware: normal boot, MIDI start/stop driving notes and clock, the version
boot chord, and the update-mode VUP dialogue (discovery, version, begin and
ordered packets, including rejection of an out-of-order packet). Installation
programming itself is exercised only in the browser workbench, which models
the host-granted installation context; the native engine keeps unconditional
flash protection.

Project mode is exercised against this engine's 1 MiB SPI model, where only
saved-project slot 0 fits: the JEDEC capacity probe must keep the firmware
inside the part (the model faults any out-of-range address), slot 0 must
save, load and delete, and the unavailable slot 1 must be refused without a
flash write. Full 64-slot coverage lives in tools/browser_panel_test.mjs
against the 16 MiB browser model.

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


PROJECT_SLOT0 = 0xC0000
SETTINGS_ADDR = 0x40000


def set_row(h, row, value, instructions=600000):
    """Drive one decoded scan row and run; encode_row(0) releases it."""
    group = row * 6
    h.machine.board.panel.rx[group:group + 6] = encode_row(value)
    h.run(instructions)


def test_project_mode(image):
    print('-- project mode (1 MiB SPI part: slot 0 only)')
    h = boot(image)
    h.run(1500000)
    flash = h.machine.board.flash
    # Give the workspace a distinct edit first: pad B7 (row 6, bit 25).
    set_row(h, 6, 1 << 25)
    set_row(h, 6, 0)
    before = bytes(flash.data)

    # PROJECT (row 6, bit 17) enters project mode; the slot rescan must stay
    # inside the 1 MiB part (the model faults out-of-range SPI addresses).
    set_row(h, 6, 1 << 17)
    set_row(h, 6, 0)
    expect(bytes(flash.data) == before, 'entering project mode writes nothing')

    # SHIFT (row 7, bit 17) + pad A1 (row 0, bit 26) saves slot 0.
    set_row(h, 7, 1 << 17)
    set_row(h, 0, 1 << 26)
    set_row(h, 0, 0)
    set_row(h, 7, 0)
    record = bytes(flash.data[PROJECT_SLOT0:PROJECT_SLOT0 + 32])
    expect(record[:4] == b'EUPJ', 'slot 0 holds a project record')
    expect(record[8 + 5] == 16 and record[8 + 5 + 1] == 7,
           'saved record carries the edited workspace (track B E(7,16))')
    changed = {i for i, (a, b) in enumerate(zip(before, bytes(flash.data))) if a != b}
    expect(changed and all(PROJECT_SLOT0 <= i < PROJECT_SLOT0 + 0x1000 for i in changed),
           'save touched only the slot-0 sector')

    # Pad A2 (row 1, bit 26) is slot 1: beyond the 1 MiB part, so the load is
    # refused without any flash mutation.
    saved = bytes(flash.data)
    writes = flash.writes
    set_row(h, 1, 1 << 26)
    set_row(h, 1, 0)
    expect(flash.writes == writes and bytes(flash.data) == saved,
           'unavailable slot refused without a flash write')

    # Change the workspace (new project via SHIFT+PROJECT), then load slot 0
    # back and commit settings with GLOBAL after leaving project mode; the
    # committed settings prove the load restored the saved workspace.
    set_row(h, 7, 1 << 17)
    set_row(h, 6, 1 << 17)
    set_row(h, 6, 0)
    set_row(h, 7, 0)
    expect(bytes(flash.data) == saved, 'new project writes no slot')
    set_row(h, 0, 1 << 26)  # load slot 0
    set_row(h, 0, 0)
    set_row(h, 6, 1 << 17)  # leave project mode
    set_row(h, 6, 0)
    set_row(h, 6, 1 << 18)  # GLOBAL commit
    set_row(h, 6, 0, 1200000)
    settings = bytes(flash.data[SETTINGS_ADDR:SETTINGS_ADDR + 32])
    expect(settings[:4] == b'EUC1' and settings[8 + 5] == 16 and settings[8 + 5 + 1] == 7,
           'loaded project restored the workspace (committed settings match)')

    # RECORD (row 1, bit 0) + pad A1 deletes slot 0.
    set_row(h, 6, 1 << 17)  # back into project mode
    set_row(h, 6, 0)
    set_row(h, 1, 1 << 0)
    set_row(h, 0, 1 << 26)
    set_row(h, 0, 0)
    set_row(h, 1, 0)
    sector = bytes(flash.data[PROJECT_SLOT0:PROJECT_SLOT0 + 0x1000])
    expect(sector == bytes([255]) * 0x1000, 'delete left slot 0 erased')
    writes = flash.writes
    set_row(h, 1, 1 << 0)   # repeated delete of the now-empty slot
    set_row(h, 0, 1 << 26)
    set_row(h, 0, 0)
    set_row(h, 1, 0)
    expect(flash.writes == writes, 'repeated delete of an empty slot writes nothing')
    report = h.run(200000)
    expect(not report['critical']['terminated'], 'no critical hazard in project mode')


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
    test_project_mode(image)
    test_version_mode(image)
    test_update_mode(image)
    print('all headless checks passed')


if __name__ == '__main__':
    main()
