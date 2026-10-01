#!/usr/bin/env node
// Renders a demo scene with the WebGPU renderer (web/gpu.js) in a headless
// Chrome and writes it as a PFM, taking photon_tracer's flags, so that
// tests/render_test.py can check it against the CPU renderer's references:
//
//   node tests/webgpu_render.mjs [--field | --mesh | --room | --crowd N] --width W --spp N --seed S --out FILE.pfm
//
// Serves web/dist (build it first: web/build.sh) and starts Chrome with
// WebGPU on Vulkan. Needs google-chrome-stable and a GPU Chrome can use.
import { spawn } from 'node:child_process';
import { createServer } from 'node:http';
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, extname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = join(dirname(fileURLToPath(import.meta.url)), '..', 'web', 'dist');
const args = process.argv.slice(2);
let scene = 0, crowd = 20, width = 960, spp = 16, seed = 1, out = 'webgpu.pfm';
for (let k = 0; k < args.length; k++) {
  const a = args[k];
  if (a === '--field') scene = 1;
  else if (a === '--mesh') scene = 2;
  else if (a === '--room') scene = 3;
  else if (a === '--crowd') { scene = 4; if (/^\d+$/.test(args[k + 1] || '')) crowd = Number(args[++k]); }
  else if (a === '--width') width = Number(args[++k]);
  else if (a === '--spp') spp = Number(args[++k]);
  else if (a === '--seed') seed = Number(args[++k]);
  else if (a === '--out') out = args[++k];
  else { console.error(`webgpu_render: unsupported flag ${a}`); process.exit(2); }
}
const height = Math.floor(width / (16 / 9));

// The page: renders and hands back the linear image, base64-encoded.
const page = `<!doctype html><meta charset="utf-8"><script type="module">
import { GpuRenderer } from './gpu.js';
window.renderScene = async (scene, crowd, width, height, spp, seed) => {
  const worker = new Worker('worker.js');
  const packed = await new Promise((resolve) => {
    worker.onmessage = ({ data }) => { if (data.type === 'scene') resolve(data.packed); };
    worker.postMessage({ type: 'export', scene, crowd });
  });
  worker.terminate();
  const r = await GpuRenderer.create();
  if (!r) throw new Error('no WebGPU adapter');
  r.loadScene(packed);
  const t0 = performance.now();
  await r.render({ width, height, spp, bounces: 10, denoise: false, seed });
  const ms = performance.now() - t0;
  const img = await r.readImage(width, height);
  const bytes = new Uint8Array(img.buffer);
  let s = '';
  for (let i = 0; i < bytes.length; i += 0x8000) s += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000));
  return JSON.stringify({ image: btoa(s), ms, rays: await r.readStats(), adapter: r.adapterInfo.architecture || r.adapterInfo.vendor });
};
window.ready = true;
</script>`;

const types = { '.js': 'text/javascript', '.wasm': 'application/wasm', '.wgsl': 'text/plain', '.html': 'text/html', '.obj': 'text/plain' };
const server = createServer((req, res) => {
  const path = decodeURIComponent(new URL(req.url, 'http://x').pathname);
  if (path === '/test.html') { res.writeHead(200, { 'content-type': 'text/html' }); res.end(page); return; }
  try {
    const body = readFileSync(join(root, path));
    res.writeHead(200, { 'content-type': types[extname(path)] || 'application/octet-stream' });
    res.end(body);
  } catch { res.writeHead(404); res.end(); }
});
await new Promise((r) => server.listen(0, '127.0.0.1', r));
const port = server.address().port;

const profile = mkdtempSync(join(tmpdir(), 'photon-webgpu-'));
const chrome = spawn('google-chrome-stable', [
  '--headless=new', `--user-data-dir=${profile}`, '--remote-debugging-port=0', '--no-first-run',
  '--enable-unsafe-webgpu', '--enable-features=Vulkan,SkiaGraphite', '--ignore-gpu-blocklist', '--use-angle=vulkan',
  'about:blank',
], { stdio: ['ignore', 'ignore', 'pipe'] });
let cleanup = () => { chrome.kill(); server.close(); rmSync(profile, { recursive: true, force: true }); };
try {
  // Chrome prints the DevTools address once it is listening.
  const devtools = await new Promise((resolve, reject) => {
    let log = '';
    chrome.stderr.on('data', (d) => {
      log += d;
      const m = log.match(/DevTools listening on (ws:\/\/\S+)/);
      if (m) resolve(m[1]);
    });
    setTimeout(() => reject(new Error('Chrome did not start')), 20000);
  });
  const httpBase = devtools.replace(/^ws:\/\/([^/]+).*/, 'http://$1');
  const target = await (await fetch(`${httpBase}/json/new?http://127.0.0.1:${port}/test.html`, { method: 'PUT' })).json();
  const ws = new WebSocket(target.webSocketDebuggerUrl);
  let id = 0;
  const waits = new Map();
  ws.onmessage = (m) => {
    const d = JSON.parse(m.data);
    if (d.id && waits.has(d.id)) { waits.get(d.id)(d); waits.delete(d.id); }
    else if (d.method === 'Runtime.exceptionThrown') console.error('page:', d.params.exceptionDetails.exception?.description);
    else if (d.method === 'Runtime.consoleAPICalled' && ['error', 'warning'].includes(d.params.type))
      console.error('page:', d.params.args.map((a) => a.value ?? a.description).join(' '));
  };
  const send = (method, params = {}) => new Promise((r) => { const i = ++id; waits.set(i, r); ws.send(JSON.stringify({ id: i, method, params })); });
  await new Promise((r) => (ws.onopen = r));
  await send('Runtime.enable');
  const evaluate = async (expression) => {
    const res = await send('Runtime.evaluate', { expression, awaitPromise: true, returnByValue: true, timeout: 600000 });
    if (res.result?.exceptionDetails) throw new Error(res.result.exceptionDetails.exception?.description || 'page error');
    return res.result.result.value;
  };
  for (let k = 0; k < 100 && !(await evaluate('window.ready === true')); k++) await new Promise((r) => setTimeout(r, 100));
  const result = JSON.parse(await evaluate(`renderScene(${scene}, ${crowd}, ${width}, ${height}, ${spp}, ${seed})`));
  const floats = new Float32Array(new Uint8Array(Buffer.from(result.image, 'base64')).buffer);
  // PFM: bottom row first, little-endian.
  const rows = [];
  for (let y = height - 1; y >= 0; y--) rows.push(Buffer.from(floats.buffer, 4 * 3 * width * y, 4 * 3 * width));
  writeFileSync(out, Buffer.concat([Buffer.from(`PF\n${width} ${height}\n-1.0\n`), ...rows]));
  console.error(`${width}x${height} at ${spp} spp: rendered in ${(result.ms / 1000).toFixed(3)} s, ` +
                `${(result.rays / (result.ms / 1000) / 1e6).toFixed(0)}M rays/s on ${result.adapter}`);
  ws.close();
} finally {
  cleanup();
}
