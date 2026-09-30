// Page logic for the photon_tracer demo: a pool of workers, a row scheduler,
// and a canvas. All tracing happens in WebAssembly inside the workers.
const $ = (id) => document.getElementById(id);
const cores = navigator.hardwareConcurrency || 4;
let pool = [], job = null;

$('threads').max = Math.max(2, cores);
$('threads').value = Math.max(1, cores);
for (const id of ['spp', 'bounces', 'threads']) {
  const show = () => { $(id + 'V').textContent = $(id).value; };
  $(id).addEventListener('input', show); show();
}

function ensurePool(n) {
  while (pool.length < n) pool.push(new Worker('worker.js'));
  while (pool.length > n) pool.pop().terminate();
}

function human(n) {
  return n >= 1e9 ? (n / 1e9).toFixed(2) + 'B' : n >= 1e6 ? (n / 1e6).toFixed(1) + 'M' : n >= 1e3 ? (n / 1e3).toFixed(1) + 'k' : String(Math.round(n));
}

// Samples per pass. Progressive: 1, 1, 2, 4... until spp is reached, so a
// noisy full frame shows up at once and then refines. Otherwise one pass.
function passSizes(spp, progressive) {
  if (!progressive) return [spp];
  const sizes = [];
  for (let total = 0, next = 1; total < spp; next = sizes.length > 1 ? next * 2 : 1) {
    const n = Math.min(next, spp - total);
    sizes.push(n); total += n;
  }
  return sizes;
}

function render() {
  if (job) stop();
  const width = Number($('res').value), height = Math.round(width * 9 / 16);
  const spp = Number($('spp').value), bounces = Number($('bounces').value);
  const scene = Number($('scene').value), bvh = $('bvh').value === '1';
  const n = Number($('threads').value), bands = $('sched').value === 'bands';
  const passes = passSizes(spp, $('prog').value === '1');
  ensurePool(n);

  const canvas = $('out');
  canvas.width = width; canvas.height = height;
  const ctx = canvas.getContext('2d');
  ctx.fillStyle = '#05080d'; ctx.fillRect(0, 0, width, height);
  // The whole frame, painted to the canvas at most once per animation frame
  // and only across the rows that changed; one putImageData per row cost the
  // main thread about a second of a 3 s render.
  const image = ctx.createImageData(width, height);
  let dirtyTop = height, dirtyBottom = -1, drawQueued = false;
  const draw = () => {
    drawQueued = false;
    if (dirtyBottom < dirtyTop) return;
    ctx.putImageData(image, 0, 0, 0, dirtyTop, width, dirtyBottom - dirtyTop + 1);
    dirtyTop = height; dirtyBottom = -1;
  };

  // Linear color summed over every sample so far, and the sample count, per row.
  const sum = new Float32Array(3 * width * height), count = new Uint16Array(height);
  const total = height * passes.length;
  job = { height, total, passes: passes.length, next: 0, done: 0, rays: 0, start: performance.now(), perWorker: new Array(n).fill(0), live: true };
  const rowsPerBand = Math.floor(height / n);
  const bandStart = pool.map((_, w) => w * rowsPerBand);
  const bandLen = pool.map((_, w) => (w === n - 1 ? height : (w + 1) * rowsPerBand) - bandStart[w]);
  const bandNext = pool.map(() => 0);

  // Work units run pass by pass; within a pass, j counts up from the bottom of
  // the image, as in the native renderer. Returns [j, pass] or null.
  const nextFor = (w) => {
    if (bands) {
      const k = bandNext[w]++;
      return k < bandLen[w] * passes.length ? [bandStart[w] + (k % bandLen[w]), Math.floor(k / bandLen[w])] : null;
    }
    const k = job.next++;
    return k < total ? [k % height, Math.floor(k / height)] : null;
  };

  const bars = $('workers'); bars.textContent = '';
  for (let w = 0; w < n; w++) bars.append(document.createElement('div'));

  const send = (worker, unit) => {
    if (unit) worker.postMessage({ j: unit[0], width, height, spp: passes[unit[1]], bounces, scene, bvh });
  };

  pool.forEach((worker, w) => {
    const current = job;
    worker.onmessage = ({ data }) => {
      if (!current.live) return;
      // Hand out the next row before doing this one's bookkeeping.
      send(worker, nextFor(w));
      const base = 3 * width * data.j, y = height - 1 - data.j, out = 4 * width * y;
      count[data.j] += data.spp;
      const scale = 1 / count[data.j];
      for (let i = 0; i < width; i++) {
        for (let c = 0; c < 3; c++) {
          sum[base + 3 * i + c] += data.pixels[3 * i + c] * data.spp;
          // Gamma 2.0, as in render_row().
          image.data[out + 4 * i + c] = 256 * Math.min(Math.sqrt(sum[base + 3 * i + c] * scale), 0.999);
        }
        image.data[out + 4 * i + 3] = 255;
      }
      dirtyTop = Math.min(dirtyTop, y); dirtyBottom = Math.max(dirtyBottom, y);
      current.done++; current.rays += data.rays; current.perWorker[w]++;
      if (current.done === current.total) { draw(); finish(current); return; }
      if (!drawQueued) {
        drawQueued = true;
        requestAnimationFrame(() => { if (current.live) { draw(); update(current); } });
      }
    };
    // Two rows in flight per worker, so each has its next row queued when it
    // finishes one instead of waiting a round trip through this thread. With
    // one, workers sat idle for most of the short 1-sample progressive passes.
    for (let k = 0; k < 2; k++) send(worker, nextFor(w));
  });
  $('go').disabled = true; $('stop').disabled = false;
  $('schedNote').textContent = bands ? `each worker owns ${rowsPerBand} consecutive rows` : 'workers take whichever row is next';
}

function update(j) {
  const s = (performance.now() - j.start) / 1000;
  $('bar').style.width = (j.done / j.total) * 100 + '%';
  $('sTime').textContent = s.toFixed(2) + ' s';
  $('sRays').textContent = human(j.rays);
  $('sRate').textContent = human(j.rays / Math.max(s, 1e-3));
  $('sRows').textContent = j.passes > 1
    ? `pass ${Math.min(j.passes, Math.floor(j.done / j.height) + 1)}/${j.passes}`
    : `${j.done}/${j.height} rows`;
  const max = Math.max(...j.perWorker, 1);
  [...$('workers').children].forEach((el, w) => { el.style.height = (j.perWorker[w] / max) * 100 + '%'; el.title = `worker ${w}: ${j.perWorker[w]} rows`; });
}

function finish(j) {
  j.live = false;
  update(j);
  $('go').disabled = false; $('stop').disabled = true;
  job = null;
}

function stop() {
  if (!job) return;
  job.live = false;
  pool.forEach((w) => w.terminate());
  pool = [];
  job = null;
  $('go').disabled = false; $('stop').disabled = true;
}

// The BVH defaults to on for the field and the teapot and off for the
// five-sphere scene, as in the native renderer: four small spheres are
// cheaper to test than a tree is to walk. Picking a scene resets it.
const bvhForScene = () => { $('bvh').value = $('scene').value === '0' ? '0' : '1'; };
$('scene').addEventListener('change', bvhForScene);

// Preselect controls from the query string, e.g. ?scene=2&spp=50, so a
// particular render can be linked to.
const params = new URLSearchParams(location.search);
for (const [key, value] of params) {
  const el = document.getElementById(key);
  if (el && (el.tagName === 'SELECT' || el.tagName === 'INPUT')) { el.value = value; el.dispatchEvent(new Event('input')); }
}
if (!params.has('bvh')) bvhForScene();

$('go').onclick = render;
$('stop').onclick = stop;
render();
