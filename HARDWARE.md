# Using this firmware on a real SQ-64

This guide covers installing `build/euclidean.vsb` on a physical Korg SQ-64,
operating it, going back to stock firmware, and what to do when something goes
wrong. Read all of it — especially [Risks](#risks-read-first) and
[Back up first](#2-back-up-the-device) — before touching hardware.

## Risks (read first)

- **The board contract is incomplete.** The independent
  [specification](../../spec) this firmware is written against explicitly
  leaves the exact MCU part, pin wiring, electrical limits and
  protected-storage ownership unverified. Passing the spec's `image` profile
  and running in the [emulator](../../emulator) is **not** a certification
  that the image is safe on a physical SQ-64.
- **Power loss while internal flash is being programmed may be
  unrecoverable** (spec requirement VUP-008). There is no verified external
  recovery path (no documented SWD/JTAG procedure). The SQ-64 is USB
  bus-powered: use a known-good cable and a stable supply, not a loose hub,
  for the entire update.
- **A final packet acknowledgement is not proof that programming finished.**
  Keep the device powered until it signals completion on its display, then
  restart and verify.
- Installing replaces the stock system firmware, including the stock
  update-mode code. Afterwards, the **only** way back to stock is this
  firmware's own updater (below) working correctly on your unit.

If any of that is unacceptable, stay in the browser emulator — everything in
this firmware can be exercised there (see `README.md` and
`scripts/`).

## What you need

- A Korg SQ-64 and a reliable USB cable/power source.
- The sibling [emulator/updater web app](../../emulator/web). Run it locally
  (`npm ci && npm run dev` in `../../emulator/web`) in a browser with Web
  MIDI + SysEx support (Chromium works). It is the host tool for firmware
  transfer (update mode) and backup/restore (normal mode).
- **Korg's official SQ-64 system updater `.VSB`**, downloaded from Korg and
  kept somewhere safe *before* you start. This is your path back to stock.
- A built and checked image: `make && make check && make test`.

## 1. Validate the image

Before any hardware step:

```sh
make            # build/euclidean.vsb
make check      # the spec's seven image-profile checks must all pass
make test       # headless + browser-engine behavioral tests
```

Then load `build/euclidean.vsb` in the browser workbench, run it, and use
**Validate Image**. If the workbench ever shows **“Do not flash this firmware
to a real device”** (a critical flash-safety hazard tripped), do not proceed
under any circumstances.

## 2. Back up the device

This firmware does not implement the stock backup/restore SysEx transport,
and its project mode writes into the **same 64-slot saved-project catalog
stock firmware uses** (SPI 0xc0000 onward, per the board contract): saving
or deleting a project slot overwrites the first 4 KiB of that stock slot
region, destroying any stock project saved there. The installer itself only
writes the regions listed in the [storage map](README.md#storage-map), and
the storage ownership map remains one of the spec's unverified areas. Take
a full backup while stock firmware is still installed:

1. Boot the SQ-64 normally and connect USB.
2. In the web app's backup tool: **Enable MIDI access**, grant SysEx
   permission, select the SQ-64 input and output ports.
3. Take a backup with scope `all` and save the archive file somewhere safe,
   off the device and outside the browser.

## 3. Install

1. Power the SQ-64 off.
2. Hold **GLOBAL + PATTERN** and power on. The stock firmware boots into its
   update mode.
3. Connect USB, open the updater web app, enable MIDI access with SysEx, and
   select the SQ-64 ports.
4. Select `build/euclidean.vsb` as the file to send. The default build is
   version **1.0**, which is *older* than stock (2.0x), so you must enable
   the host tool's explicit reinstall/downgrade option. Whether the stock
   device-side updater itself refuses older version numbers on real hardware
   is unverified; if the transfer is rejected at the begin step, rebuild with
   a higher version instead:

   ```sh
   make clean && make VERSION_MAJOR=3 VERSION_MINOR=0 && make check
   ```

   (Limits: major < 64, minor < 128.)
5. Send the firmware. The transfer is 224 packets of 1024 bytes; the host
   waits for an acknowledgement after every packet and does not retry on
   error. **Do not disconnect or power off.**
6. When all packets are acknowledged, keep the device powered and watch its
   display until the stock updater indicates the update is complete, then
   restart it (power off/on).

### Verify

Hold **GLOBAL** (alone) and power on. The display should read:

```
EUCLID SQ-64
VERSION 1.0
ORIGINAL EUCLIDEAN FIRMWARE
```

Press the power button to shut down, then boot normally and confirm the
default state: track A selected, pattern E(4,16) on the pad LEDs, STOP LED
lit, 120 BPM.

## 4. Operating the firmware

Boot modes (checked once at power-on, via the panel scan):

| Held at power-on | Mode |
| --- | --- |
| nothing | Normal sequencer |
| GLOBAL | Version display |
| GLOBAL + PATTERN | Update mode (`UPDATE MODE / WAITING FOR HOST`) |

Normal-mode controls are in [README.md](README.md#controls-normal-mode).
Connections:

- **Tracks A/B/C** → their gate jacks, pitch CV and mod CV outputs, and MIDI
  note output on channels 1, 2 and 3.
- **Track D** → trigger jack 1 and MIDI notes on channel 10.
- **MIDI out** carries clock while playing, plus start/stop. Incoming MIDI
  start/stop is honored; incoming clock sync is **not** implemented, nor is
  the analog sync jack.

Projects:

- **PROJECT** toggles project mode: the 64 pads become the 64 saved project
  slots (bank A pad 1 = slot 1 … bank D pad 16 = slot 64; lit = saved). Pad
  loads a slot, SHIFT+pad saves the workspace to it, RECORD+pad deletes it,
  and SHIFT+PROJECT starts a new project from the declared defaults. Empty
  or invalid slots are reported on the display and never touch the
  workspace; SAVED/DELETED appear only after the flash contents read back
  verified. **Slots are shared with the stock saved-project catalog** — see
  the backup warning above.

Persistence:

- **GLOBAL** commits the current settings (patterns, tempo, swing, notes) to
  SPI flash.
- The **power button** saves and shuts down cleanly through the power hold.
  Always shut down with the power button rather than cutting power —
  although a settings commit is a single 4 KiB sector and the firmware is
  otherwise idle on flash, there is no reason to interrupt a write.

What the firmware will and will not write is listed in the
[storage map](README.md#storage-map). Note that explicit project saves and
deletes write into the stock saved-project catalog; nothing else does, and
nothing writes outside the listed regions.

## 5. Going back to stock

1. Power off. Hold **GLOBAL + PATTERN** and power on. The display shows
   `UPDATE MODE / WAITING FOR HOST`.
2. Connect USB and send **Korg's official `.VSB`** with the updater web app,
   exactly as in the install procedure. The display walks through
   `RECEIVING UPDATE` → `PROGRAMMING / DO NOT REMOVE POWER` →
   `UPDATE COMPLETE / RESTART THE DEVICE`.
3. Power cycle, verify the stock version screen, then restore your backup
   with the backup tool (normal mode). After a restore, shut down with the
   power button and wait for power-off before restarting, then compare a
   fresh backup to confirm persistence.

The same procedure installs a newer build of this firmware
(self-update). The updater buffers the entire payload in RAM, validates
geometry, reset vectors and all four region checksums, and only then
programs flash from a RAM-resident installer — any error or interruption
**before the final packet** leaves all storage untouched.

## Troubleshooting

**No MIDI ports in the browser.** Check the cable, that the browser granted
SysEx permission, and that no other application holds the MIDI port. The
device enumerates USB in update mode; give it a moment after boot. The
updater also listens on DIN MIDI, if you have a DIN interface.

**Transfer fails or is rejected mid-way.** The host does not retry
automatically. If the failure happened before the final packet was sent,
no flash has been written: power cycle back into update mode and start the
transfer again from the beginning. This firmware's updater rejects with a
negative acknowledgement on: malformed begin/packet (format), nonzero block,
out-of-order packet index, bad packet checksum, bad version fields, data
outside an accepted begin, and any install attempt outside update mode
(version mode answers version queries but never programs).

**Power lost after the final packet, during `PROGRAMMING`.** This is the
case VUP-008 warns about. Try the boot chords; if the device no longer
reaches update mode, there is no verified recovery procedure. This is the
risk you accepted in the warning section.

**Device misbehaves after install.** Boot version mode (GLOBAL) to confirm
what is actually installed; from update mode you can always reinstall this
firmware or restore stock, as long as the updater still boots.

## What has and has not been tested

Everything above the hardware boundary is exercised automatically: the spec's
`image` profile, a full end-to-end 224-packet installation under the
emulator's flash-safety monitors, and the panel/outputs/persistence behavior
(see [Tests](README.md#tests)). Quantitative timing (jitter, latency, CV
calibration) and electrical behavior require hardware measurement and are
not claimed. Treat yourself as the first hardware tester.
