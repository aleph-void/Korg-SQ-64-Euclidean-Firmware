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
  rotation, 1–64 steps in four bars of 16 (polymetric against each other),
  plus a per-track note, gate length, mod CV level and velocity.
- Tracks A/B/C: gate jack, pitch CV, mod CV and MIDI notes on channels 1–3.
  Track D: trigger jack 1 and MIDI notes on channel 10.
- Tempo 20–300 BPM with tap tempo, swing 50–75 %, MIDI clock/start/stop out,
  and MIDI start/stop in (external clock *sync* is not implemented).
- The spec's panel-key requirements (TRACK/MODE/LOOP/BAR/TAP/TEMPO/SWING-001): A–D
  select the edited track; PITCH, GATE, MOD and CONTROL each toggle an edit
  mode scoping the pads to that one parameter of the selected track (note,
  gate %, mod CV level, velocity); keys 1–4 select which 16-step bar the
  pads, pad LEDs and display lanes address; LOOP loops playback over the
  selected bar of the selected track's pattern (refused when that bar lies
  beyond the pattern, clipped or cleared if a later edit moves the pattern
  end) and clearing it resumes the free-running counter. Selection, mode
  entry/exit, bar keys and loop toggles alone never change stored data.
- No boot splash is declared (spec SPLASH-001): the display goes straight to
  the sequencer UI, and the boot chords are honored regardless.
- Pad LEDs show the selected bar of each track's pattern with a moving
  playhead; the display shows all four lanes plus the edited track's
  parameters in the active edit mode, and the mode/bar/loop status.
- Settings persist to SPI flash (GLOBAL commits; the power button saves and
  shuts down cleanly through the PB7 power hold).
- Full project lifecycle over the board contract's 64-slot saved-project
  catalog (spec PROJECT-001..004): PROJECT enters project mode, where the 64
  pads map 1:1 to the 64 slots (bank A pad 1 = slot 0 … bank D pad 16 =
  slot 63). Pad = load, SHIFT+pad = save, RECORD+pad = delete,
  SHIFT+PROJECT = new project (workspace back to the declared defaults).
  Slot LEDs show the catalog; empty and invalid slots are reported on the
  display and never change the workspace; save/delete confirmations appear
  only after the stored bytes read back verified. The fitted SPI capacity is
  probed via JEDEC, and slots beyond it are refused rather than aliased.
- Boot chords per the spec: hold **GLOBAL** at power-on for version mode,
  **GLOBAL+PATTERN** for update mode. In version mode the PLAY indicator is
  lit, and pressing **PLAY** continues the normal boot sequence to the
  ready state (BOOT-004).
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
| PLAY / STOP | Start from step 0 (or the loop start) / stop (notes off, gates cleared) |
| TAP | Tap tempo |
| A B C D | Select the edited track |
| PITCH / GATE / MOD / CONTROL | Toggle that edit mode (same key again returns to EUCLID) |
| 1 2 3 4 | Select the bar the pads address (pad *n* = step (bar−1)×16+*n*) |
| LOOP | Toggle a loop over the selected bar of the selected track's pattern |
| Pad *n* (any bank) | EUCLID: pulses = selected step. PITCH: note 36+*n*−1. GATE: 5+(*n*−1)×6 %. MOD: level *n*−1. CONTROL: velocity (*n*−1)×8+7 |
| SHIFT + pad *n* | EUCLID mode only: set that track's length to the selected step |
| Tempo / swing encoders | BPM / swing |
| Param 1–4 encoders | Pulses, length, rotation, note |
| GLOBAL | Commit settings to SPI flash |
| PROJECT | Enter / leave project mode |
| SHIFT + PROJECT | New project: workspace back to the declared defaults |
| Power button | Save and shut down |

Declared SHIFT combinations (spec SHIFT-001): SHIFT+pad in EUCLID mode
(length), SHIFT+pad in project mode (save) and SHIFT+PROJECT (new project).
SHIFT alone, and SHIFT with any other control — including pads in the PITCH,
GATE, MOD and CONTROL modes — has no effect.

In project mode the pads stop editing patterns and address the 64 saved
project slots instead (slot = bank × 16 + pad − 1):

| Control | Action |
| --- | --- |
| Pad *n* | Load that slot (an empty or invalid slot is reported, workspace unchanged) |
| SHIFT + pad *n* | Save the workspace to that slot |
| RECORD + pad *n* | Delete that slot |
| PLAY / STOP / TAP | Transport keeps working |

A project records the whole workspace: all four track patterns, tempo,
swing and the selected track. Loading one replaces the workspace; it does
not touch the committed settings until the next GLOBAL commit or
power-button shutdown.

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
Every build runs `tools/check_blocks.py`, which fails the link when any
branch-free instruction run exceeds 80 instructions: the browser engine's
patched TCI
interpreter has a fixed translation budget per basic block (about 120 simple
instructions, fewer for load/store-heavy code) and a longer block traps the
WASM host before it executes (see the spec's contracts page, "Known
browser-engine limit"). `tools/make_vsb.py` wraps the 64 KiB binary into a
spec-conformant VSB, filling unused payload with 0xFF and balancing the four
region checksums.

## Run it

Browser workbench (recommended): in `../../emulator/web`, `npm ci && npm run
dev`, open the printed URL, load `build/euclidean.vsb` in **Browser
emulator**, and run. Boot chords: double-click latches a button, then press
Power on. Use **Send a second firmware image** after booting update mode to
exercise the updater; the usual critical flash guards stay active.

Importable demo scripts for the workbench's **UI scripts** panel live in
[`scripts/`](scripts/README.md); they walk through every
major function (transport, tap tempo, pattern editing, polymeter, swing,
MIDI remote start/stop, settings persistence, project management, version
mode and a full self-update).

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
  The edit-key section covers the panel-key requirements headlessly:
  track, mode, bar and loop keys are selection-only (zero flash writes), and
  pad edits land on the selected track through the active mode and bar,
  verified in the committed settings.
  Project mode runs against this engine's 1 MiB SPI model: the JEDEC
  capacity probe must confine the catalog to slot 0 (the model faults any
  out-of-range address), which is saved, loaded back, deleted and
  re-deleted, while the unavailable slot 1 is refused without a write.
- `tools/browser_update_test.mjs` (browser WASM engine): full end-to-end
  installation — update-mode boot via the panel chord, USB MIDI enumeration,
  all 224 packets through the firmware's own updater under the workbench's
  installation context, then byte-exact verification of internal flash and
  the three installed SPI regions, with no critical hazard tripped.
- `tools/browser_panel_test.mjs` (browser WASM engine): DMA panel scanning,
  pad/encoder/transport input, pattern LEDs, gate/trigger/CV observers, MIDI
  output, GLOBAL commit, power-button shutdown, and settings persistence
  across a cold restart. The panel-key requirements run end to end: each edit
  mode's LED, scoping and exit, mode edits audible in the MIDI note/velocity
  and mod CV output with zero storage writes, bar windowing of the pad LEDs
  over a 20-step pattern, a loop over the selected bar proven in the note
  stream (and silent again once cleared), and a loop refusal beyond the
  active pattern's bounds. The full project lifecycle runs against the 16 MiB
  model: saves to the first, middle and last slot with byte-exact
  untouched-slot guards, exact load-back, empty- and corrupted-slot
  rejection with the workspace proven unchanged, delete (and repeated
  delete) confined to its own sector, new-project defaults with no writes,
  and catalog persistence across a cold restart.

## Storage map

| Region | Use |
| --- | --- |
| Internal flash 0x00200000 + 64 KiB | This firmware (VSB payload 0x00000) |
| SPI 0x00000–0x30000 | Installed-image regions; written only by the updater during a host-initiated installation |
| SPI 0x40000 (one 4 KiB sector) | Committed settings (magic `EUC2`, FNV-1a checked); rewritten only when contents change |
| SPI 0xc0000–0x1000000 | The board contract's 64 saved-project slots, 0x3d000 apart. A project record (magic `EUP2`, FNV-1a checked) occupies only the first 4 KiB sector of its slot, written solely by an explicit save or delete in project mode. **This is the same catalog stock firmware keeps its saved projects in**, so saving over a slot overwrites whatever a stock project left there — back up first (see HARDWARE.md) |
| Everything else | Never written |

A save erases and reprograms one 4 KiB sector; an interruption between the
erase and the program leaves that slot observably empty or invalid (reported
on load, never loaded), with every other slot, the workspace and settings
untouched — save again to recover. Unchanged saves and deletes of
already-empty slots skip the erase/program cycle entirely.

## Provenance

Original work. Interface facts (container format, update protocol, panel
wiring, peripheral behavior) come from the sibling spec's `board.json`,
`contracts.md` and the emulator's independently authored board model — see
`src/board.h` for the consolidated register map. The 5×7 font and all other
content are drawn from scratch. Licensed under the MIT License (see LICENSE).

## Known limits

- The stock backup/restore SysEx transport is not implemented; settings and
  the saved-project slots are this firmware's only persistent data. Project
  records use this firmware's own compact format (`EUP2`), stored at the
  stock catalog's slot addresses — stock firmware cannot read them, nor this
  firmware stock projects (a stock-written slot shows as INVALID and is never
  loaded).
- External clock sync (analog sync jack, incoming MIDI clock) is not
  implemented; incoming start/stop are honored.
- MIDI input notes are ignored; the device is a sequencer, not a sound module.
- Quantitative timing claims (jitter, latency) require hardware measurement
  and are not made.
