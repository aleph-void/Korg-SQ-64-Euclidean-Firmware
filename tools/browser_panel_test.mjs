#!/usr/bin/env node
// Normal-mode interaction test against the browser (WASM) engine: panel DMA
// scanning, pads, encoders, transport, LEDs, gates, CV slots, MIDI output,
// and the spec panel-key requirements (edit modes, bar selection, loop).
//
// Usage: node tools/browser_panel_test.mjs [path/to/emulator/web] [image.vsb]
import {readFileSync} from 'node:fs';
import {pathToFileURL} from 'node:url';
import {resolve} from 'node:path';

const webRoot = resolve(process.argv[2] || new URL('../../../emulator/web', import.meta.url).pathname);
const imagePath = resolve(process.argv[3] || new URL('../build/euclidean.vsb', import.meta.url).pathname);
const load = path => import(pathToFileURL(resolve(webRoot, path)));

const {default: createUnicorn} = await load('vendor/unicorn-arm.js');
const {Machine} = await load('emulator/core.js');
const {loadImage} = await load('emulator/image.js');
const {outputLevels} = await load('emulator/outputs.js');

const failures = [];
const check = (condition, label) => {
  console.log((condition ? 'ok  ' : 'FAIL') + ' ' + label);
  if (!condition) failures.push(label);
};

const engine = await createUnicorn();
const midi = [];
const machine = new Machine(engine, await loadImage(new Uint8Array(readFileSync(imagePath))),
  {onMidi: m => midi.push(Array.from(m))});
const run = n => {
  while (n > 0) {
    const report = machine.run(Math.min(n, 200000));
    if (report.error) throw Error('emulator: ' + report.error);
    n -= 200000;
  }
};
const press = (control, instructions = 400000) => {
  machine.event({type: 'button', control, down: true});
  run(instructions);
  machine.event({type: 'button', control, down: false});
  run(instructions);
};

run(1500000); // boot into normal mode
check(machine.board.panel.displayFrames > 0, 'display rendered');
const idleLeds = {...machine.board.panel.leds};
check(idleLeds.trackA > 128, 'selected-track LED lit');
check(idleLeds.A1 > 128 && idleLeds.A5 > 128 && idleLeds.A2 < 64,
  'track A default pattern E(4,16) on pad LEDs');
check(idleLeds.stop > 128 && idleLeds.play < 64, 'stop LED lit while stopped');

// Start playback from the panel.
press('play');
run(6000000); // almost a second of virtual time at 120 BPM
const playing = outputLevels(machine.board);
check(midi.some(m => m[0] === 0xfa), 'MIDI start sent');
check(midi.some(m => m[0] === 0xf8), 'MIDI clock sent');
check(midi.some(m => (m[0] & 0xf0) === 0x90), 'note-ons sent');
check(machine.board.panel.leds.play > 128, 'play LED lit');
const cvActive = ['Amod', 'Apitch'].every(k => playing[k] !== undefined) || true;
check(playing.Apitch > 0, `track A pitch CV level ${playing.Apitch}`);
let gateSeen = playing.Agate === 255;
for (let i = 0; i < 20 && !gateSeen; i++) { run(400000); gateSeen = outputLevels(machine.board).Agate === 255; }
check(gateSeen, 'track A gate observed active');
let trigSeen = false;
for (let i = 0; i < 30 && !trigSeen; i++) { run(200000); trigSeen = outputLevels(machine.board).D1 === 255; }
check(trigSeen, 'track D trigger 1 observed active');

press('stop');
run(2000000);
check(midi.some(m => m[0] === 0xfc), 'MIDI stop sent');
const stopped = outputLevels(machine.board);
check(stopped.Agate === 0 && stopped.Bgate === 0 && stopped.Cgate === 0 && stopped.D1 === 0,
  'gates and triggers inactive after stop');

// Pad B7 sets track B pulses to 7 and selects track B.
press('B7');
run(1000000);
const ledsB = machine.board.panel.leds;
check(ledsB.trackB > 128 && ledsB.trackA < 64, 'pad press selected track B');
const bOn = Array.from({length: 16}, (_, i) => ledsB['B' + (i + 1)] > 128).filter(Boolean).length;
check(bOn === 7, `track B shows 7 pulses on LEDs (got ${bOn})`);

// Tempo encoder: +5 detents.
machine.event({type: 'encoder', control: 'tempo', delta: 5});
run(4000000);
// Param1 (pulses) encoder: -2 detents on track B.
machine.event({type: 'encoder', control: 'param1', delta: -2});
run(4000000);
const bAfter = Array.from({length: 16}, (_, i) => machine.board.panel.leds['B' + (i + 1)] > 128).filter(Boolean).length;
check(bAfter === 5, `pulses encoder reduced track B to 5 pulses (got ${bAfter})`);

const slotAddr = n => 0xc0000 + n * 0x3d000;
const hold = (control, down) => { machine.event({type: 'button', control, down}); run(400000); };
const chord = (modifier, control) => { hold(modifier, true); press(control); hold(modifier, false); };
const bPulses = m => Array.from({length: 16}, (_, i) => m.board.panel.leds['B' + (i + 1)] > 128).filter(Boolean).length;
const spiDiff = (before, after, allowed) => {
  for (let i = 0; i < before.length; i++) {
    if (before[i] === after[i]) continue;
    if (!allowed.some(([start, end]) => i >= start && i < end)) return i;
  }
  return -1;
};
const noteOns = (from, channel) =>
  midi.slice(from).filter(m => (m[0] & 0xf0) === 0x90 && (m[0] & 15) === channel);

// --- Edit modes (MODE-001): PITCH/GATE/MOD/CONTROL scope the pads to one
// parameter of the selected track; entry, edits and exit write no storage. ---
const flashBeforeModes = machine.board.flash.data.slice();
const writesBeforeModes = machine.board.flash.writes;
press('pitch');
check(machine.board.panel.leds.pitch > 128, 'pitch mode LED lit');
press('B8'); // note 43 (G2)
press('pitch');
check(machine.board.panel.leds.pitch < 64, 'second press leaves pitch mode');
press('control');
check(machine.board.panel.leds.control > 128, 'control mode LED lit');
press('B16'); // velocity 127
press('control');
press('gate');
check(machine.board.panel.leds.gate > 128, 'gate mode LED lit');
press('B15'); // gate 89 % of a step
press('gate');
press('mod');
check(machine.board.panel.leds.mod > 128, 'mod mode LED lit');
press('B16'); // mod level 15 (full scale)
press('mod');
check(bPulses(machine) === 5, 'mode edits left the pattern pulses alone');
check(machine.board.flash.writes === writesBeforeModes
  && spiDiff(flashBeforeModes, machine.board.flash.data, []) === -1,
  'mode entry, edits and exit wrote nothing to storage');

// The mode edits are audible: track B now plays note 43 at velocity 127
// with a full-scale mod CV while its gate is open.
const modeMark = midi.length;
press('play');
run(4000000);
const bOns = noteOns(modeMark, 1);
check(bOns.length > 0 && bOns.every(m => m[1] === 43 && m[2] === 127),
  `track B plays the pitch and velocity set in the edit modes (${bOns.length} notes)`);
let modSeen = false;
for (let i = 0; i < 40 && !modSeen; i++) { run(200000); modSeen = outputLevels(machine.board).Bmod === 255; }
check(modSeen, 'track B mod CV reaches the mod-mode level');
press('stop');
run(1000000);

// --- Bar selection (BAR-001): keys 1..4 window the pads onto one 16-step
// bar of the 64-step pattern space. ---
press('bar2');
const barLeds = machine.board.panel.leds;
check(barLeds.bar2 > 128 && barLeds.bar1 < 64, 'bar 2 selected and shown');
check(bPulses(machine) === 0, 'bar 2 of a 16-step pattern shows no steps');
chord('shift', 'B4'); // SHIFT+pad in bar 2: length = step 20
check(bPulses(machine) === 1, 'E(5,20) shows exactly its bar-2 step (17)');
press('bar1');
check(machine.board.panel.leds.bar1 > 128, 'bar 1 reselected');
check(bPulses(machine) === 4, 'bar 1 of E(5,20) shows its four steps');

// --- Loop (LOOP-001): LOOP loops the selected bar of the active pattern;
// clearing it resumes normal playback. ---
press('B1'); // pulses = 1: track B fires only at step 1 (inside bar 1)
press('bar2');
press('loop');
check(machine.board.panel.leds.loop > 128, 'loop LED lit');
const loopMark = midi.length;
press('play');
run(8000000);
press('stop');
run(500000);
const loopB = noteOns(loopMark, 1).length, loopA = noteOns(loopMark, 0).length;
check(loopB === 0 && loopA > 0,
  `loop over bar 2 keeps playback off track B's bar-1 pulse (A ${loopA}, B ${loopB})`);
press('loop');
check(machine.board.panel.leds.loop < 64, 'loop cleared');
const clearMark = midi.length;
press('play');
run(8000000);
press('stop');
run(500000);
check(noteOns(clearMark, 1).length > 0, 'clearing the loop resumes normal playback');

// A loop whose selected bar lies beyond the active pattern is refused.
press('trackA');
check(machine.board.panel.leds.trackA > 128, 'track key selects track A');
press('loop'); // bar 2 is beyond track A's 16 steps
check(machine.board.panel.leds.loop < 64, 'loop beyond the pattern bounds refused');

// Restore the workspace (track B, E(5,16)) for the project checks below.
press('bar1');
chord('shift', 'B16');
press('B5');
check(bPulses(machine) === 5 && machine.board.panel.leds.trackB > 128,
  'workspace restored to track B E(5,16)');

// --- Project mode: the 64 pads manage the 64 saved slots (16 MiB model). ---
const spiBefore = machine.board.flash.data.slice();
press('project');
check(machine.board.panel.leds.project > 128, 'project mode LED lit');
const emptyLit = ['A1', 'C1', 'D16'].filter(k => machine.board.panel.leds[k] > 128);
check(emptyLit.length === 0, 'no slot LEDs lit over an empty catalog');

// Save the workspace (track B, 5 pulses) to the first, middle and last slot.
chord('shift', 'A1');
chord('shift', 'C1');
chord('shift', 'D16');
const leds = machine.board.panel.leds;
check(leds.A1 > 128 && leds.C1 > 128 && leds.D16 > 128, 'saved slots 0/32/63 lit');
const rec = machine.board.flash.data.subarray(slotAddr(0), slotAddr(0) + 40);
check(rec[0] === 0x45 && rec[1] === 0x55 && rec[2] === 0x50 && rec[3] === 0x33,
  'slot 0 record magic EUP3');
check(rec[15] === 16 && rec[16] === 5, 'slot 0 record holds track B E(5,16)');
const sectors = [0, 32, 63].map(n => [slotAddr(n), slotAddr(n) + 4096]);
const stray = spiDiff(spiBefore, machine.board.flash.data, sectors);
check(stray === -1, `saves touched only the three slot sectors (stray @0x${stray.toString(16)})`);

// Change the workspace, then prove load restores it exactly.
press('project'); // leave project mode
press('B9');
run(500000);
check(bPulses(machine) === 9, 'workspace edited to track B E(9,16)');
press('project');
press('A2'); // empty slot: refused, workspace untouched
press('project');
run(500000);
check(bPulses(machine) === 9, 'empty-slot load left the workspace unchanged');

// An invalid (corrupted) slot is reported, never loaded.
machine.board.flash.data[slotAddr(0) + 4] ^= 0xff;
press('project'); // re-enter: rescan
check(machine.board.panel.leds.A1 < 64, 'corrupted slot 0 no longer lit');
press('A1');
press('project');
run(500000);
check(bPulses(machine) === 9, 'invalid-slot load left the workspace unchanged');
machine.board.flash.data[slotAddr(0) + 4] ^= 0xff;

press('project');
press('A1'); // load slot 0
press('project');
run(500000);
check(bPulses(machine) === 5 && machine.board.panel.leds.trackB > 128,
  'loading slot 0 restored track B E(5,16)');

// Delete the middle slot; everything else survives.
const beforeDelete = machine.board.flash.data.slice();
press('project');
chord('record', 'C1');
check(machine.board.panel.leds.C1 < 64, 'deleted slot LED off');
const erased = machine.board.flash.data.subarray(slotAddr(32), slotAddr(32) + 4096);
check(erased.every(b => b === 255), 'deleted slot sector erased');
const strayDel = spiDiff(beforeDelete, machine.board.flash.data, [[slotAddr(32), slotAddr(32) + 4096]]);
check(strayDel === -1, 'delete touched only its own sector');
const writesAfterDelete = machine.board.flash.writes;
chord('record', 'C1'); // repeated delete of an empty slot
check(machine.board.flash.writes === writesAfterDelete, 'repeated delete writes nothing');

// New project: workspace back to declared defaults, saved slots untouched.
const beforeNew = machine.board.flash.data.slice();
chord('shift', 'project');
press('project'); // leave project mode
run(500000);
const newLeds = machine.board.panel.leds;
check(newLeds.trackA > 128 && newLeds.A1 > 128 && newLeds.A5 > 128 && newLeds.A2 < 64,
  'new project restored the default workspace E(4,16)');
check(spiDiff(beforeNew, machine.board.flash.data, []) === -1, 'new project wrote nothing');

// Back to the saved workspace so the settings-persistence checks below
// still see track B with 5 pulses.
press('project');
press('A1');
press('project');
run(500000);
check(bPulses(machine) === 5, 'reloaded slot 0 before shutdown');

// Commit with GLOBAL, then power off and verify the hold release.
const writesBefore = machine.board.flash.writes;
press('global');
run(2000000);
check(machine.board.flash.writes > writesBefore, 'GLOBAL committed settings to SPI flash');

machine.event({type: 'power', down: true});
run(4000000);
check(machine.board.poweredOff, 'power button shutdown releases the hold');

// Cold restart with the same SPI flash contents: committed settings persist.
const savedSpi = machine.board.flash.data.slice();
const machine2 = new Machine(engine, await loadImage(new Uint8Array(readFileSync(imagePath))), {onMidi: () => {}});
machine2.board.flash.data.set(savedSpi);
let n = 1500000;
while (n > 0) { const r = machine2.run(200000); if (r.error) throw Error(r.error); n -= 200000; }
const restored = Array.from({length: 16}, (_, i) => machine2.board.panel.leds['B' + (i + 1)] > 128).filter(Boolean).length;
check(restored === 5 && machine2.board.panel.leds.trackB > 128,
  `committed settings restored after restart (track B, ${restored} pulses)`);

// Saved projects survive the cold restart too: slots 0 and 63 populated,
// deleted slot 32 still empty, and slot 0 loads back.
const run2 = n => { while (n > 0) { const r = machine2.run(200000); if (r.error) throw Error(r.error); n -= 200000; } };
const press2 = control => {
  machine2.event({type: 'button', control, down: true});
  run2(400000);
  machine2.event({type: 'button', control, down: false});
  run2(400000);
};
press2('project');
const leds2 = machine2.board.panel.leds;
check(leds2.A1 > 128 && leds2.D16 > 128 && leds2.C1 < 64,
  'slot catalog persisted across cold restart (0 and 63 full, 32 empty)');
press2('B12'); // slot 27 is empty: load refused
press2('A1');  // slot 0 loads
press2('project');
run2(500000);
check(bPulses(machine2) === 5 && machine2.board.panel.leds.trackB > 128,
  'slot 0 loaded after cold restart');
machine2.close();

machine.close();
if (failures.length) { console.error('FAILED: ' + failures.join('; ')); process.exit(1); }
console.log('browser-engine panel test passed');
