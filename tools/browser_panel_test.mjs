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
machine2.close();

machine.close();
if (failures.length) { console.error('FAILED: ' + failures.join('; ')); process.exit(1); }
console.log('browser-engine panel test passed');
