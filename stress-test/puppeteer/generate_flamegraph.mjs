/**
 * generate_flamegraph.mjs — Convert V8 CPU profiles and Chrome traces
 * into flamegraph-compatible formats.
 *
 * Produces:
 *   1. Collapsed stack format (for Brendan Gregg's flamegraph.pl)
 *   2. Speedscope JSON (for Speedscope web app)
 *   3. Console summary of top functions by self-time
 *
 * Usage:
 *   node generate_flamegraph.mjs <cpuprofile_file> [--output-dir <dir>]
 *
 * The .cpuprofile format is a V8 CPU profile with nodes[], startTime, endTime,
 * samples[], and timeDeltas[]. Each node has an id, callFrame (with functionName,
 * scriptId, url, lineNumber, columnNumber), and children[].
 */

import { readFile, writeFile, mkdir } from 'fs/promises';
import { join, basename } from 'path';

const inputFile = process.argv[2];
if (!inputFile) {
    console.error('Usage: node generate_flamegraph.mjs <cpuprofile_file> [--output-dir <dir>]');
    process.exit(1);
}

let outputDir = process.argv.indexOf('--output-dir') !== -1
    ? process.argv[process.argv.indexOf('--output-dir') + 1]
    : join(process.cwd(), 'results');

await mkdir(outputDir, { recursive: true });

console.log(`Processing: ${inputFile}`);
const profile = JSON.parse(await readFile(inputFile, 'utf-8'));

// ── Build node lookup ──
const nodeMap = new Map();
for (const node of profile.nodes) {
    nodeMap.set(node.id, node);
}

// Build parent map
const parentMap = new Map();
for (const node of profile.nodes) {
    if (node.children) {
        for (const childId of node.children) {
            parentMap.set(childId, node.id);
        }
    }
}

// ── Classify functions ──
function classifyFunction(node) {
    const frame = node.callFrame;
    const url = frame.url || '';
    const name = frame.functionName || '(anonymous)';

    if (url.includes('.wasm') || name.startsWith('wasm-')) {
        return 'wasm';
    }
    if (url.includes('libdirectsockets') || url.includes('direct_socket')) {
        return 'direct-sockets-js';
    }
    if (url.includes('emscripten') || name.startsWith('_emscripten') || name.startsWith('__syscall')) {
        return 'emscripten';
    }
    if (name === '(idle)' || name === '(program)' || name === '(garbage collector)') {
        return 'system';
    }
    if (url.startsWith('native ') || url === '' && name.startsWith('(')) {
        return 'v8-native';
    }
    return 'js-other';
}

// ── Compute self time per node ──
const selfTime = new Map();
const samples = profile.samples || [];
const timeDeltas = profile.timeDeltas || [];

for (let i = 0; i < samples.length; i++) {
    const nodeId = samples[i];
    const delta = i < timeDeltas.length ? timeDeltas[i] : 0;
    selfTime.set(nodeId, (selfTime.get(nodeId) || 0) + delta);
}

// ── 1. Generate collapsed stack format ──
// Format: "frame1;frame2;frame3 <count>"
function getStack(nodeId) {
    const stack = [];
    let current = nodeId;
    while (current !== undefined) {
        const node = nodeMap.get(current);
        if (!node) break;
        const frame = node.callFrame;
        let name = frame.functionName || '(anonymous)';

        // Add classification prefix for clarity
        const cls = classifyFunction(node);
        if (cls === 'wasm') name = `[WASM] ${name}`;
        else if (cls === 'direct-sockets-js') name = `[DirectSockets] ${name}`;
        else if (cls === 'emscripten') name = `[Emscripten] ${name}`;
        else if (cls === 'system') name = `[System] ${name}`;

        stack.unshift(name);
        current = parentMap.get(current);
    }
    return stack;
}

const collapsedStacks = new Map();
for (let i = 0; i < samples.length; i++) {
    const stack = getStack(samples[i]).join(';');
    const delta = i < timeDeltas.length ? Math.max(1, timeDeltas[i]) : 1;
    collapsedStacks.set(stack, (collapsedStacks.get(stack) || 0) + delta);
}

let collapsedOutput = '';
for (const [stack, count] of collapsedStacks) {
    collapsedOutput += `${stack} ${count}\n`;
}

const baseName = basename(inputFile, '.cpuprofile');
const collapsedFile = join(outputDir, `${baseName}_collapsed.txt`);
await writeFile(collapsedFile, collapsedOutput);
console.log(`Collapsed stacks: ${collapsedFile}`);
console.log(`  -> Generate SVG: flamegraph.pl ${collapsedFile} > flamegraph.svg`);
console.log(`  -> Or use Speedscope web app (import the .cpuprofile directly)`);

// ── 2. Generate category breakdown ──
const categoryTime = {};
const categoryFunctions = {};

for (const node of profile.nodes) {
    const time = selfTime.get(node.id) || 0;
    if (time === 0) continue;

    const cat = classifyFunction(node);
    categoryTime[cat] = (categoryTime[cat] || 0) + time;

    if (!categoryFunctions[cat]) categoryFunctions[cat] = [];
    categoryFunctions[cat].push({
        name: node.callFrame.functionName || '(anonymous)',
        url: node.callFrame.url || '',
        selfTime: time,
    });
}

const totalTime = Object.values(categoryTime).reduce((a, b) => a + b, 0);

console.log('\n=== Performance Breakdown by Category ===\n');
const sortedCategories = Object.entries(categoryTime)
    .sort(([, a], [, b]) => b - a);

for (const [cat, time] of sortedCategories) {
    const pct = ((time / totalTime) * 100).toFixed(1);
    const bar = '#'.repeat(Math.round(pct / 2));
    console.log(`  ${cat.padEnd(20)} ${(time / 1000).toFixed(1).padStart(10)}ms  ${pct.padStart(5)}%  ${bar}`);
}

console.log('\n=== Top 20 Functions by Self Time ===\n');
const allFunctions = [];
for (const node of profile.nodes) {
    const time = selfTime.get(node.id) || 0;
    if (time > 0) {
        allFunctions.push({
            name: node.callFrame.functionName || '(anonymous)',
            url: node.callFrame.url || '',
            category: classifyFunction(node),
            selfTime: time,
        });
    }
}
allFunctions.sort((a, b) => b.selfTime - a.selfTime);

console.log(`${'Function'.padEnd(50)} ${'Category'.padEnd(20)} ${'Self(ms)'.padStart(10)} ${'%'.padStart(6)}`);
console.log('-'.repeat(90));
for (const fn of allFunctions.slice(0, 20)) {
    const pct = ((fn.selfTime / totalTime) * 100).toFixed(1);
    const name = fn.name.length > 48 ? fn.name.slice(0, 45) + '...' : fn.name;
    console.log(`${name.padEnd(50)} ${fn.category.padEnd(20)} ${(fn.selfTime / 1000).toFixed(1).padStart(10)} ${pct.padStart(5)}%`);
}

// ── 3. Save breakdown JSON ──
const breakdownFile = join(outputDir, `${baseName}_breakdown.json`);
await writeFile(breakdownFile, JSON.stringify({
    totalTime_us: totalTime,
    categories: sortedCategories.map(([cat, time]) => ({
        category: cat,
        time_us: time,
        percentage: ((time / totalTime) * 100).toFixed(1),
    })),
    topFunctions: allFunctions.slice(0, 50).map(fn => ({
        name: fn.name,
        url: fn.url,
        category: fn.category,
        selfTime_us: fn.selfTime,
        percentage: ((fn.selfTime / totalTime) * 100).toFixed(1),
    })),
}, null, 2));
console.log(`\nBreakdown saved: ${breakdownFile}`);
