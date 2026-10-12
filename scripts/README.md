# UI demo scripts

Portable `sq64-ui-script` files for the browser workbench's **UI scripts**
panel. Together they demonstrate every major function of this firmware in the
emulator — no hardware involved.

## Running a script

1. In `../../../../emulator/web`: `npm ci && npm run dev`, open the printed
   URL and expand **Browser emulator**.
2. Load `build/euclidean.vsb` (build it first with `make`).
3. Expand **UI scripts** below the instrument panel, choose **Import
   script**, select one of these files, then **Run script**.

Scripts drive the normal emulator input path, so you can watch the pads,
LEDs, display, and the top-edge gate/CV/trigger indicators respond live.
Each script that boots the instrument waits **20 s** after `power-on` for
the firmware to start scanning the panel (first display frame took about 4 s
on the reference host); on a slow machine or a throttled background tab,
raise the `wait` after each `power-on` before reporting a problem. An
instance keeps running after a script finishes unless the script powers it
off.

## The scripts

| Script | Demonstrates | Watch for |
| --- | --- | --- |
| `01-transport-and-tap.json` | PLAY, STOP, tap tempo | Play LED and moving playhead; four TAP presses ~420 ms apart raise the BPM readout; STOP clears gates |
| `02-pattern-editing.json` | Track select, pad = pulses, SHIFT+pad = length, param encoders 1–4 (pulses, length, rotation, note) | Pad B7 selects track B with E(7,16); SHIFT+B12 trims length to 12; rotation shifts the pattern; note +12 transposes an octave; then it plays |
| `03-tempo-and-swing.json` | Tempo and swing encoders while playing | BPM and swing % on the display; audible-in-LEDs shuffle while swing is up |
| `04-four-track-polymeter.json` | Four independent E(pulses, length) tracks with different lengths | A=E(5,8), B=E(3,13), C=E(7,16), D=E(2,4) drifting against each other; track D fires the trigger indicators, A–C the gate/CV ones |
| `05-midi-remote-start-stop.json` | MIDI start (0xFA) / stop (0xFC) input | Transport starts and stops with no button press |
| `06-save-and-persistence.json` | GLOBAL settings commit, power-button shutdown, persistence | Sets track B to 9 pulses, commits, powers off, reboots: track B comes back selected with 9 pulses |
| `07-version-mode.json` | GLOBAL boot chord | `EUCLID SQ-64 / VERSION 1.0 / ORIGINAL EUCLIDEAN FIRMWARE`, then a clean power-off |
| `08-update-mode-install.json` | GLOBAL+PATTERN boot chord and the full VUP self-updater | See below |
| `09-project-management.json` | Project mode: save, load, empty-slot report, delete, new project | Pads become the 64 slot selectors (lit = saved): B7 is saved to slot A1 and D16, loading empty B3 reports `B3 EMPTY`, the pattern is edited away and loading A1 brings it back, RECORD+D16 deletes, SHIFT+PROJECT resets the workspace to defaults |
| `10-modes-loop-bars.json` | The panel-key requirements: PITCH/GATE/MOD/CONTROL edit modes, bar selection, the loop | Each mode key lights its LED and the pads retarget one parameter of track B (note, gate %, mod level — watch the B MOD indicator jump to full — velocity); key 2 selects bar 2 and SHIFT+B4 stretches the pattern to 20 steps; LOOP pins playback inside that bar until cleared; key 1 returns to bar 1 |

## Notes on script 08 (updater)

Before running it, expand **Send a second firmware image** and select a
complete `.VSB` there (for a harmless demonstration, select
`build/euclidean.vsb` itself — `allowOlder` is set because reinstalling the
same version is not an upgrade). The `firmware-update` action sends that
file through the firmware's own updater: the display walks through
`UPDATE MODE / WAITING FOR HOST` → `RECEIVING UPDATE` →
`PROGRAMMING / DO NOT REMOVE POWER` → `UPDATE COMPLETE / RESTART THE
DEVICE`. The action waits up to two minutes for USB enumeration first, so
the short startup wait is intentional. The workbench's critical flash-safety
guards stay active throughout. Installation writes the emulated flash only;
the booted image stays loaded, so a later `power-on` reboots the original.

Script 05 needs the emulator's USB MIDI route up before the injected bytes
land; if the transport does not react, increase its post-boot wait (USB
configuration took up to a minute on the reference host).

These scripts are demonstrations, not assertions — the repeatable behavioral
checks live in `tools/headless_test.py`, `tools/browser_panel_test.mjs` and
`tools/browser_update_test.mjs` (see `make test`).
