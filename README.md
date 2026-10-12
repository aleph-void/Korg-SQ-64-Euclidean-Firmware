# Euclidean — original SQ-64 firmware

A from-scratch firmware for the Korg SQ-64 hardware sequencer that replaces
the stock sequencer with a four-track **Euclidean rhythm sequencer**. It is
written entirely from the interface facts published by the independent
[SQ-64 firmware specification](../../spec) and exercised against the
independent [SQ-64 emulator](../../emulator). **It contains no Korg source
code, no decompiled vendor functions, no vendor assets and no bytes copied
from stock firmware.**

> **Hardware warning.** The specification's board contract is explicitly
> incomplete: exact MCU part, wiring, electrical limits and protected-storage
> ownership remain unverified release blockers. This image passes the spec's
> `image` profile and runs in the emulator; that is **not** a certification
> that it is safe on a physical SQ-64. Flashing real hardware is at your own
> risk, and power loss while internal flash is being programmed is not
> guaranteed to be recoverable (spec VUP-008). If you accept that,
> [HARDWARE.md](HARDWARE.md) documents the full install / operate / restore
> procedure.

## Features

- Four tracks, each an independent Euclidean pattern E(pulses, length) with
  rotation, 1–16 steps (polymetric against each other).
- Tracks A/B/C: gate jack, pitch CV, mod CV and MIDI notes on channels 1–3.
  Track D: trigger jack 1 and MIDI notes on channel 10.
- Tempo 20–300 BPM with tap tempo, swing 50–75 %, MIDI clock/start/stop out,
  and MIDI start/stop in (external clock *sync* is not implemented).
- Pad LEDs show each track's pattern with a moving playhead; the display
  shows all four lanes plus the edited track's parameters.
- Settings persist to SPI flash (GLOBAL commits; the power button saves and
  shuts down cleanly through the PB7 power hold).
- Boot chords per the spec: hold **GLOBAL** at power-on for version mode,
  **GLOBAL+PATTERN** for update mode.
- A complete self-updater in update mode (VUP SysEx protocol 1, 1024-byte
  packets, over USB MIDI or DIN): it buffers the whole 0x38000-byte system
  payload in RAM, validates geometry, reset vectors and all four region
  checksums, and only then programs internal flash and the installed SPI
  regions — from a RAM-resident installer, since programming erases the flash
  the updater booted from. Any error or interruption before the final packet
  leaves storage untouched.

## Controls (normal mode)

| Control | Action |
| --- | --- |
| PLAY / STOP | Start from step 0 / stop (notes off, gates cleared) |
| TAP | Tap tempo |
| A B C D | Select the edited track |
| Pad *n* (any bank) | Set that track's pulses to *n* |
| SHIFT + pad *n* | Set that track's length to *n* |
| Tempo / swing encoders | BPM / swing |
| Param 1–4 encoders | Pulses, length, rotation, note |
| GLOBAL | Commit settings to SPI flash |
| Power button | Save and shut down |

## Build

Requires the GNU Arm toolchain (`arm-none-eabi-gcc`) and Python 3.

```sh
make            # build/euclidean.elf, .bin and .vsb
make check      # run the spec's seven image-profile checks on the VSB
make test       # headless + browser-engine behavioral tests (see below)
```

The code is compiled as the ARMv6-M subset so it runs on both emulator
engines (the native engine's stock Unicorn build cannot deliver interrupts
inside IT blocks); `make ARCH=-mcpu=cortex-m7` builds Thumb-2 instead.
`tools/make_vsb.py` wraps the 64 KiB binary into a spec-conformant VSB,
filling unused payload with 0xFF and balancing the four region checksums.

## Run it

Browser workbench (recommended): in `../../emulator/web`, `npm ci && npm run
dev`, open the printed URL, load `build/euclidean.vsb` in **Browser
emulator**, and run. Boot chords: double-click latches a button, then press
Power on. Use **Send a second firmware image** after booting update mode to
exercise the updater; the usual critical flash guards stay active.

Importable demo scripts for the workbench's **UI scripts** panel live in
[`scripts/`](scripts/README.md); they walk through every
major function (transport, tap tempo, pattern editing, polymeter, swing,
MIDI remote start/stop, settings persistence, version mode and a full
self-update).

To put it on a physical SQ-64 — including backing up first and getting back
to stock firmware — see [HARDWARE.md](HARDWARE.md) and its warnings.

Native headless:

```sh
python -m pip install -e ../../emulator
make smoke      # 2M instructions, report in build/smoke.json
```

## Tests

- `tools/headless_test.py` (native engine): boot, tick interrupts, MIDI
  start/clock/notes/stop, version and update boot chords, VUP discovery /
  version / begin, ordered packets, and rejection of out-of-order packets,
  wrong blocks, and installs outside update mode — with zero flash writes.
- `tools/browser_update_test.mjs` (browser WASM engine): full end-to-end
  installation — update-mode boot via the panel chord, USB MIDI enumeration,
  all 224 packets through the firmware's own updater under the workbench's
  installation context, then byte-exact verification of internal flash and
  the three installed SPI regions, with no critical hazard tripped.
- `tools/browser_panel_test.mjs` (browser WASM engine): DMA panel scanning,
  pad/encoder/transport input, pattern LEDs, gate/trigger/CV observers, MIDI
  output, GLOBAL commit, power-button shutdown, and settings persistence
  across a cold restart.

## Storage map

| Region | Use |
| --- | --- |
| Internal flash 0x00200000 + 64 KiB | This firmware (VSB payload 0x00000) |
| SPI 0x00000–0x30000 | Installed-image regions; written only by the updater during a host-initiated installation |
| SPI 0x40000 (one 4 KiB sector) | Committed settings (magic `EUC1`, FNV-1a checked); rewritten only when contents change |
| Everything else | Never written |

## Provenance

Original work. Interface facts (container format, update protocol, panel
wiring, peripheral behavior) come from the sibling spec's `board.json`,
`contracts.md` and the emulator's independently authored board model — see
`src/board.h` for the consolidated register map. The 5×7 font and all other
content are drawn from scratch. Licensed under the MIT License (see LICENSE).

## Known limits

- The stock backup/restore SysEx transport is not implemented; settings are
  this firmware's only persistent data.
- External clock sync (analog sync jack, incoming MIDI clock) is not
  implemented; incoming start/stop are honored.
- MIDI input notes are ignored; the device is a sequencer, not a sound module.
- Quantitative timing claims (jitter, latency) require hardware measurement
  and are not made.
