#!/usr/bin/env node
// Normal-mode interaction test against the browser (WASM) engine: panel DMA
// scanning, pads, encoders, transport, LEDs, gates, CV slots and MIDI output.
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

// --- Project mode: the 64 pads manage the 64 saved slots (16 MiB model). ---
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
const rec = machine.board.flash.data.subarray(slotAddr(0), slotAddr(0) + 32);
check(rec[0] === 0x45 && rec[1] === 0x55 && rec[2] === 0x50 && rec[3] === 0x4a,
  'slot 0 record magic EUPJ');
check(rec[13] === 16 && rec[14] === 5, 'slot 0 record holds track B E(5,16)');
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
