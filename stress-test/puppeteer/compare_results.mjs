/**
 * compare_results.mjs — Compare WASM vs Native stress test results
 * and generate comparison charts as HTML + SVG.
 *
 * Usage:
 *   node compare_results.mjs <wasm_results_dir> <native_results_dir> [--output <file.html>]
 *
 * Reads combined_results.json from each directory and produces:
 *   - Side-by-side latency comparison table
 *   - Inline SVG bar charts for p50/p95/p99
 *   - Throughput comparison
 *   - Overhead ratio (WASM/Native)
 */

import { readFile, writeFile, readdir } from 'fs/promises';
import { join } from 'path';

const wasmDir = process.argv[2];
const nativeDir = process.argv[3];
const outputFile = process.argv.indexOf('--output') !== -1
    ? process.argv[process.argv.indexOf('--output') + 1]
    : 'comparison_report.html';

if (!wasmDir || !nativeDir) {
    console.error('Usage: node compare_results.mjs <wasm_results_dir> <native_results_dir>');
    process.exit(1);
}

async function loadResults(dir) {
    const combined = join(dir, 'combined_results.json');
    try {
        return JSON.parse(await readFile(combined, 'utf-8'));
    } catch {
        console.error(`Could not read ${combined}`);
        return [];
    }
}

const wasmResults = await loadResults(wasmDir);
const nativeResults = await loadResults(nativeDir);

// Match tests by label
const nativeByLabel = new Map();
for (const r of nativeResults) {
    nativeByLabel.set(r.label, r);
}

// ── Build comparison data ──
const comparisons = [];
for (const w of wasmResults) {
    const n = nativeByLabel.get(w.label);
    if (!n) continue;

    comparisons.push({
        label: w.label,
        wasm: w,
        native: n,
        overhead_p50: (w.p50_us / n.p50_us).toFixed(2),
        overhead_p95: (w.p95_us / n.p95_us).toFixed(2),
        overhead_p99: (w.p99_us / n.p99_us).toFixed(2),
    });
}

// ── Console output ──
console.log('\n=== WASM vs Native Comparison ===\n');
console.log(`${'Test'.padEnd(35)} ${'WASM p50'.padStart(10)} ${'Native p50'.padStart(10)} ${'Ratio'.padStart(8)}  ${'WASM p95'.padStart(10)} ${'Native p95'.padStart(10)} ${'Ratio'.padStart(8)}`);
console.log('-'.repeat(100));

for (const c of comparisons) {
    console.log(
        `${c.label.padEnd(35)} ` +
        `${c.wasm.p50_us.toString().padStart(9)}u ${c.native.p50_us.toString().padStart(9)}u ${c.overhead_p50.padStart(7)}x  ` +
        `${c.wasm.p95_us.toString().padStart(9)}u ${c.native.p95_us.toString().padStart(9)}u ${c.overhead_p95.padStart(7)}x`
    );
}

// ── Generate HTML report ──
function svgBarChart(data, width, height) {
    // data: [{label, wasm, native}]
    const maxVal = Math.max(...data.flatMap(d => [d.wasm, d.native]));
    const barHeight = Math.floor((height - 40) / (data.length * 2));
    const labelWidth = 200;
    const chartWidth = width - labelWidth - 80;

    let svg = `<svg width="${width}" height="${height}" xmlns="http://www.w3.org/2000/svg">\n`;
    svg += `<style>
        text { font-family: 'SF Mono', monospace; font-size: 11px; fill: #e9f1ff; }
        .label { font-size: 10px; fill: #888; }
        .value { font-size: 10px; }
    </style>\n`;

    let y = 20;
    for (const d of data) {
        const wasmW = (d.wasm / maxVal) * chartWidth;
        const nativeW = (d.native / maxVal) * chartWidth;

        // Label
        svg += `<text x="0" y="${y + barHeight - 2}" class="label">${d.label}</text>\n`;

        // WASM bar
        svg += `<rect x="${labelWidth}" y="${y}" width="${wasmW}" height="${barHeight - 2}" fill="#0ff" opacity="0.7" rx="2"/>\n`;
        svg += `<text x="${labelWidth + wasmW + 5}" y="${y + barHeight - 3}" class="value" fill="#0ff">${d.wasm}us</text>\n`;
        y += barHeight;

        // Native bar
        svg += `<rect x="${labelWidth}" y="${y}" width="${nativeW}" height="${barHeight - 2}" fill="#0f0" opacity="0.7" rx="2"/>\n`;
        svg += `<text x="${labelWidth + nativeW + 5}" y="${y + barHeight - 3}" class="value" fill="#0f0">${d.native}us</text>\n`;
        y += barHeight + 4;
    }

    // Legend
    svg += `<rect x="${width - 150}" y="5" width="12" height="12" fill="#0ff" opacity="0.7" rx="2"/>`;
    svg += `<text x="${width - 133}" y="15" class="value" fill="#0ff">WASM</text>`;
    svg += `<rect x="${width - 80}" y="5" width="12" height="12" fill="#0f0" opacity="0.7" rx="2"/>`;
    svg += `<text x="${width - 63}" y="15" class="value" fill="#0f0">Native</text>`;

    svg += '</svg>';
    return svg;
}

const p50Data = comparisons.map(c => ({ label: c.label, wasm: c.wasm.p50_us, native: c.native.p50_us }));
const p95Data = comparisons.map(c => ({ label: c.label, wasm: c.wasm.p95_us, native: c.native.p95_us }));

const chartHeight = Math.max(300, comparisons.length * 50 + 40);

const html = `<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<title>QUIC Stress Test: WASM vs Native</title>
<style>
    :root {
        --bg: #03030a;
        --panel: rgba(10, 14, 30, 0.85);
        --border: rgba(151, 179, 255, 0.18);
        --text: #e9f1ff;
        --dim: rgba(227, 237, 255, 0.5);
        --cyan: #0ff;
        --green: #0f0;
        --pink: #ff3b8e;
    }
    body {
        font-family: 'Space Grotesk', 'Helvetica Neue', sans-serif;
        background: var(--bg);
        color: var(--text);
        margin: 0;
        padding: 2rem;
    }
    h1 { color: var(--cyan); font-size: 1.8rem; margin-bottom: 0.5rem; }
    h2 { color: var(--dim); font-size: 1.1rem; margin-top: 2rem; }
    .panel {
        background: var(--panel);
        border: 1px solid var(--border);
        border-radius: 16px;
        padding: 1.5rem;
        margin: 1rem 0;
        overflow-x: auto;
    }
    table {
        border-collapse: collapse;
        width: 100%;
        font-size: 0.85rem;
    }
    th, td {
        padding: 0.5rem 0.75rem;
        text-align: right;
        border-bottom: 1px solid rgba(255,255,255,0.05);
    }
    th { color: var(--dim); font-weight: 500; text-transform: uppercase; font-size: 0.7rem; letter-spacing: 0.1em; }
    td:first-child, th:first-child { text-align: left; }
    .ratio { color: var(--pink); font-weight: 600; }
    .ratio.good { color: var(--green); }
    .wasm-val { color: var(--cyan); }
    .native-val { color: var(--green); }
    .chart-container { margin: 1rem 0; }
    .timestamp { color: var(--dim); font-size: 0.75rem; }
</style>
</head>
<body>
<h1>QUIC Echo Server: WASM vs Native</h1>
<p class="timestamp">Generated: ${new Date().toISOString()}</p>
<p>ngtcp2 + wolfSSL compiled to WebAssembly (Emscripten + JSPI + Direct Sockets) vs native Linux binary.</p>

<h2>Latency Comparison (p50)</h2>
<div class="panel chart-container">
${svgBarChart(p50Data, 900, chartHeight)}
</div>

<h2>Latency Comparison (p95)</h2>
<div class="panel chart-container">
${svgBarChart(p95Data, 900, chartHeight)}
</div>

<h2>Detailed Results</h2>
<div class="panel">
<table>
<tr>
    <th>Test</th>
    <th>WASM p50</th>
    <th>Native p50</th>
    <th>Ratio</th>
    <th>WASM p95</th>
    <th>Native p95</th>
    <th>Ratio</th>
    <th>WASM p99</th>
    <th>Native p99</th>
    <th>Ratio</th>
</tr>
${comparisons.map(c => `<tr>
    <td>${c.label}</td>
    <td class="wasm-val">${c.wasm.p50_us}us</td>
    <td class="native-val">${c.native.p50_us}us</td>
    <td class="ratio ${parseFloat(c.overhead_p50) < 2 ? 'good' : ''}">${c.overhead_p50}x</td>
    <td class="wasm-val">${c.wasm.p95_us}us</td>
    <td class="native-val">${c.native.p95_us}us</td>
    <td class="ratio ${parseFloat(c.overhead_p95) < 2 ? 'good' : ''}">${c.overhead_p95}x</td>
    <td class="wasm-val">${c.wasm.p99_us}us</td>
    <td class="native-val">${c.native.p99_us}us</td>
    <td class="ratio ${parseFloat(c.overhead_p99) < 2 ? 'good' : ''}">${c.overhead_p99}x</td>
</tr>`).join('\n')}
</table>
</div>

<h2>Summary Statistics</h2>
<div class="panel">
<table>
<tr><th>Metric</th><th>Value</th></tr>
<tr><td>Tests compared</td><td>${comparisons.length}</td></tr>
<tr><td>Average p50 overhead (WASM/Native)</td><td class="ratio">${(comparisons.reduce((a,c) => a + parseFloat(c.overhead_p50), 0) / comparisons.length).toFixed(2)}x</td></tr>
<tr><td>Average p95 overhead</td><td class="ratio">${(comparisons.reduce((a,c) => a + parseFloat(c.overhead_p95), 0) / comparisons.length).toFixed(2)}x</td></tr>
<tr><td>Average p99 overhead</td><td class="ratio">${(comparisons.reduce((a,c) => a + parseFloat(c.overhead_p99), 0) / comparisons.length).toFixed(2)}x</td></tr>
</table>
</div>

</body>
</html>`;

await writeFile(outputFile, html);
console.log(`\nReport saved: ${outputFile}`);
