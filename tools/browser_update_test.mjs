#!/usr/bin/env node
// End-to-end installation test against the browser (WASM) engine.
//
// Boots the Euclidean firmware with the GLOBAL+PATTERN chord held, waits for
// USB enumeration, then performs a complete VUP transfer of a version-bumped
// copy of the same image through the firmware's own updater, with the host
// installation context exactly as the browser workbench grants it. Verifies
// the acknowledgements, the programmed internal flash and SPI bytes, and that
// no critical hazard fired.
//
// Usage: node tools/browser_update_test.mjs [path/to/emulator/web] [image.vsb]
import {readFileSync} from 'node:fs';
import {pathToFileURL} from 'node:url';
import {resolve} from 'node:path';

const webRoot = resolve(process.argv[2] || new URL('../../../emulator/web', import.meta.url).pathname);
const imagePath = resolve(process.argv[3] || new URL('../build/euclidean.vsb', import.meta.url).pathname);
const load = path => import(pathToFileURL(resolve(webRoot, path)));

const {default: createUnicorn} = await load('vendor/unicorn-arm.js');
const {Machine} = await load('emulator/core.js');
const {loadImage} = await load('emulator/image.js');
const {parseVsb, transferPlan, frame, discovery, versionReply, ack, sha256} = await load('src/protocol.js');

const failures = [];
const check = (condition, label) => {
  console.log((condition ? 'ok  ' : 'FAIL') + ' ' + label);
  if (!condition) failures.push(label);
};

const vsb = new Uint8Array(readFileSync(imagePath));
const engine = await createUnicorn();
const replies = [];
const machine = new Machine(engine, await loadImage(vsb), {onMidi: m => replies.push(Array.from(m))});

// Hold the update chord through reset scanning.
machine.event({type: 'button', control: 'global', down: true});
machine.event({type: 'button', control: 'pattern', down: true});

function run(budget, until = () => false) {
  while (budget > 0 && !until()) {
    const report = machine.run(Math.min(budget, 200000));
    if (report.error) throw Error('emulator: ' + report.error);
    budget -= 200000;
  }
}

run(3000000, () => machine.board.usb.phase === 'configured');
check(machine.board.usb.phase === 'configured', 'USB device enumerated in update mode');
machine.event({type: 'button', control: 'global', down: false});
machine.event({type: 'button', control: 'pattern', down: false});

let progressFrames = 0;
async function exchange(message, budget = 4000000) {
  let seen = replies.length;
  machine.event({type: 'midi', data: Array.from(message), port: 'usb'});
  for (;;) {
    run(budget, () => replies.length > seen);
    if (replies.length === seen) throw Error('no reply to ' + message[6]);
    const reply = Uint8Array.from(replies[replies.length - 1]);
    if (reply[6] === 0x13) { progressFrames++; seen = replies.length; continue; }
    return reply;
  }
}

const caps = discovery(await exchange(frame(0)));
check(caps.protocol === 1 && caps.packetSize === 1024 && caps.flags === 0,
  `discovery: protocol ${caps.protocol}, ${caps.packetSize}-byte packets, flags ${caps.flags}`);
const installed = versionReply(await exchange(frame(2, [0])));
check(installed[0] === 1 && installed[1] === 0, `installed version ${installed.join('.')}`);

// Candidate: the same image with the header minor version bumped.
const candidateBytes = vsb.slice();
candidateBytes[0x2b] = installed[1] + 1;
const pkg = parseVsb(candidateBytes);
const plan = transferPlan(pkg, caps, installed, false);
check(plan.count === 224, `transfer plan: ${plan.count} packets`);

machine.prepareUpdate(pkg, caps, await sha256(pkg.data));
ack(await exchange(plan.begin));
for (let i = 0; i < plan.count; i++) {
  const reply = await exchange(plan.packet(i), i === plan.count - 1 ? 60000000 : 4000000);
  const status = ack(reply);
  if (status.progress) throw Error('expected acknowledgement, got progress');
}
check(true, 'all 224 packets acknowledged, including post-programming final ACK');
check(progressFrames > 0, `progress frames received (${progressFrames})`);

const installation = machine.critical.installation.report();
check(installation.phase === 'installing' && installation.packetsSubmitted === 224,
  `installation context: ${installation.phase}, ${installation.packetsSubmitted} packets`);
check(installation.mutationAttempted === true, 'firmware performed the programming itself');
check(!machine.critical.hit, 'no critical hazard tripped');

const internal = machine.mem.read(0x200000, 0x10000);
check(internal.every((b, i) => b === pkg.payload[i]), 'internal flash matches candidate payload');
const spi = machine.board.flash.data;
const expected = new Uint8Array(0x30000).fill(255);
for (const [src, dst, len] of [[0x10000, 0, 0x4000], [0x34000, 0x4000, 0x4000], [0x14000, 0x10000, 0x20000]])
  expected.set(pkg.payload.subarray(src, src + len), dst);
check(expected.every((b, i) => b === spi[i]), 'installed SPI regions match candidate payload');
check(machine.board.panel.displayFrames > 0, `display frames rendered (${machine.board.panel.displayFrames})`);

machine.close();
if (failures.length) { console.error('FAILED: ' + failures.join('; ')); process.exit(1); }
console.log('browser-engine installation test passed');
