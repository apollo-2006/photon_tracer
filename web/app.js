// Page logic for the photon_tracer demo. Two engines: the WebGPU tracer
// (gpu.js), which traces on the graphics card a scene the C++ code built, and
// the original one, a pool of WebAssembly workers fed rows by a scheduler
// here. WebGPU is the default where the browser has it.
const $ = (id) => document.getElementById(id);
const cores = navigator.hardwareConcurrency || 4;
let pool = [], job = null;
// The denoiser runs in its own worker (denoise.js), kept across renders.
// denoiseFor is the render whose frames it may still show.
let denoiser = null, denoiseFor = null, denoiseSeq = 0;

$('threads').max = Math.max(2, cores);
$('threads').value = Math.max(1, cores);
for (const id of ['spp', 'bounces', 'threads']) {
  const show = () => { $(id + 'V').textContent = $(id).value; };
  $(id).addEventListener('input', show); show();
}

// The WebGPU renderer, or null where the browser has none (or it failed).
const gpuReady = import('./gpu.js')
  .then(({ GpuRenderer }) => GpuRenderer.create())
  .catch((e) => { console.warn('WebGPU unavailable:', e); return null; });
let gpu = null, gpuContext = null, sceneWorker = null;
// Packed scenes for the GPU, by scene number, from a worker's WebAssembly.
const packedScenes = new Map();

function packedScene(scene) {
  if (packedScenes.has(scene)) return packedScenes.get(scene);
  sceneWorker = sceneWorker || new Worker('worker.js');
  const p = new Promise((resolve) => {
    const listen = ({ data }) => {
      if (data.type !== 'scene' || data.scene !== scene) return;
      sceneWorker.removeEventListener('message', listen);
      resolve(data.packed);
    };
    sceneWorker.addEventListener('message', listen);
    sceneWorker.postMessage({ type: 'export', scene });
  });
  packedScenes.set(scene, p);
  return p;
}

const usingGpu = () => gpu && $('engine').value === 'gpu';

// The controls that only mean something for the workers.
function showEngine() {
  const g = usingGpu();
  $('out').hidden = g;
  $('gpuout').hidden = !g;
  for (const id of ['bvh', 'threads', 'sched']) $(id).disabled = g;
  $('workers').hidden = g;
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
  showEngine();
  if (usingGpu()) { renderGpu(); return; }
  const width = Number($('res').value), height = Math.round(width * 9 / 16);
  const spp = Number($('spp').value), bounces = Number($('bounces').value);
  const scene = Number($('scene').value), bvh = $('bvh').value === '1';
  const n = Number($('threads').value), bands = $('sched').value === 'bands';
  const passes = passSizes(spp, $('prog').value === '1');
  const denoising = $('denoise').value === '1';
  // Samples taken before each pass, so each pass draws new ones, and one seed
  // for the whole render, shared by every worker.
  const firstSample = passes.map((_, p) => passes.slice(0, p).reduce((a, b) => a + b, 0));
  const seed = Math.floor(Math.random() * 2 ** 52);
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

  // For the denoiser, summed the same way: each pixel's first-hit albedo and
  // normal and its squared luminance, from which its noise follows.
  const px = width * height;
  const albedoSum = denoising ? new Float32Array(3 * px) : null;
  const normalSum = denoising ? new Float32Array(3 * px) : null;
  const lum2Sum = denoising ? new Float32Array(px) : null;
  const passRows = new Array(passes.length).fill(0);
  // Once a denoised frame has been drawn, noisy rows are no longer painted
  // over it; the next denoised frame replaces it.
  let showDenoised = false, denoiseBusy = false, denoisePending = false;
  const token = {};
  denoiseFor = token;
  const lum = (r, g, b) => 0.2126 * r + 0.7152 * g + 0.0722 * b;

  const requestDenoise = () => {
    if (denoiseBusy) { denoisePending = true; return; }
    denoiseBusy = true; denoisePending = false;
    const id = ++denoiseSeq;
    const color = new Float32Array(3 * px), albedo = new Float32Array(3 * px);
    const normal = new Float32Array(3 * px), variance = new Float32Array(px);
    for (let j = 0; j < height; j++) {
      const k = count[j];
      if (!k) continue;  // Not reached yet: black, and the filter keeps it apart
      for (let i = 0; i < width; i++) {
        const p = j * width + i;
        for (let c = 0; c < 3; c++) {
          color[3 * p + c] = sum[3 * p + c] / k;
          albedo[3 * p + c] = albedoSum[3 * p + c] / k;
          normal[3 * p + c] = normalSum[3 * p + c] / k;
        }
        const l = lum(color[3 * p], color[3 * p + 1], color[3 * p + 2]);
        // Variance of the mean: that of one sample over the sample count.
        variance[p] = (lum2Sum[p] / k - l * l) / k;
      }
    }
    denoiser = denoiser || new Worker('denoise.js');
    denoiser.onmessage = ({ data }) => {
      // A reply to an earlier render, or to a request since superseded.
      if (denoiseFor !== token || data.id !== id) return;
      denoiseBusy = false;
      // Rows are stored bottom first, as j counts; the canvas is top first.
      for (let j = 0; j < height; j++) {
        const out = 4 * width * (height - 1 - j);
        for (let i = 0; i < 3 * width; i++) {
          const v = 256 * Math.min(Math.sqrt(Math.max(data.out[3 * width * j + i], 0)), 0.999);
          image.data[out + 4 * Math.floor(i / 3) + (i % 3)] = v;
        }
        for (let i = 0; i < width; i++) image.data[out + 4 * i + 3] = 255;
      }
      showDenoised = true;
      ctx.putImageData(image, 0, 0);
      if (denoisePending) requestDenoise();
      else if (passRows[passes.length - 1] === height) $('sRows').textContent = `pass ${passes.length}/${passes.length}, denoised in ${Math.round(data.ms)} ms`;
    };
    denoiser.postMessage({ id, width, height, color, albedo, normal, variance },
                         [color.buffer, albedo.buffer, normal.buffer, variance.buffer]);
  };

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
    if (unit) worker.postMessage({ j: unit[0], pass: unit[1], width, height, spp: passes[unit[1]], first: firstSample[unit[1]], seed, bounces, scene, bvh, aux: denoising });
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
          // Gamma 2.0, as in to_display().
          if (!showDenoised) image.data[out + 4 * i + c] = 256 * Math.min(Math.sqrt(sum[base + 3 * i + c] * scale), 0.999);
        }
        image.data[out + 4 * i + 3] = 255;
      }
      if (denoising) {
        const g = data.guide, p0 = width * data.j;
        for (let i = 0; i < width; i++) {
          for (let c = 0; c < 3; c++) {
            albedoSum[3 * (p0 + i) + c] += g[7 * i + c] * data.spp;
            normalSum[3 * (p0 + i) + c] += g[7 * i + 3 + c] * data.spp;
          }
          lum2Sum[p0 + i] += g[7 * i + 6] * data.spp;
        }
        if (++passRows[data.pass] === height) requestDenoise();
      }
      if (!showDenoised) { dirtyTop = Math.min(dirtyTop, y); dirtyBottom = Math.max(dirtyBottom, y); }
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

// The WebGPU path: the scene packed by the C++ code, traced in batches of
// samples by a compute shader, shown after each batch.
async function renderGpu() {
  const width = Number($('res').value), height = Math.round(width * 9 / 16);
  const spp = Number($('spp').value), bounces = Number($('bounces').value);
  const scene = Number($('scene').value), denoise = $('denoise').value === '1';
  const progressive = $('prog').value === '1';
  const current = { live: true, gpu: true };
  job = current;
  $('go').disabled = true; $('stop').disabled = false;
  $('schedNote').textContent = `WebGPU on ${gpu.adapterInfo.description || gpu.adapterInfo.architecture || gpu.adapterInfo.vendor || 'this GPU'}: one invocation per pixel, the BVH always on`;
  $('sRows').textContent = 'loading scene';
  const packed = await packedScene(scene);
  if (!current.live) return;
  if (!packed) {
    // Too deep for the GPU tracer's stack: render with the workers instead.
    job = null;
    $('engine').value = 'cpu';
    render();
    return;
  }
  gpu.loadScene(packed);

  const canvas = $('gpuout');
  canvas.width = width; canvas.height = height;
  if (!gpuContext) {
    gpuContext = canvas.getContext('webgpu');
    gpuContext.configure({ device: gpu.device, format: gpu.format, alphaMode: 'opaque' });
  }
  const start = performance.now();
  const show = (done) => {
    $('bar').style.width = (done / spp) * 100 + '%';
    $('sTime').textContent = ((performance.now() - start) / 1000).toFixed(2) + ' s';
    $('sRows').textContent = `${done}/${spp} spp`;
  };
  const finished = await gpu.render({
    width, height, spp, bounces, denoise, seed: Math.floor(Math.random() * 2 ** 32),
    onStep: (done) => { if (progressive || done === spp) gpu.draw(gpuContext, width, height, denoise); show(done); },
    next: progressive ? requestAnimationFrame : null,
  });
  if (!finished || !current.live) return;
  const s = (performance.now() - start) / 1000;
  const rays = await gpu.readStats();
  show(spp);
  $('sTime').textContent = s.toFixed(2) + ' s';
  $('sRays').textContent = human(rays);
  $('sRate').textContent = human(rays / Math.max(s, 1e-3));
  if (denoise) $('sRows').textContent = `${spp}/${spp} spp, denoised`;
  current.live = false;
  job = null;
  $('go').disabled = false; $('stop').disabled = true;
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
  denoiseFor = null;
  if (gpu) gpu.stop();
  if (!job) return;
  job.live = false;
  if (!job.gpu) {
    pool.forEach((w) => w.terminate());
    pool = [];
  }
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
$('engine').addEventListener('change', () => { stop(); showEngine(); });
gpuReady.then((g) => {
  gpu = g;
  if (!gpu) {
    // No WebGPU here: the workers it is.
    $('engine').value = 'cpu';
    $('engine').querySelector('option[value="gpu"]').disabled = true;
    $('engine').querySelector('option[value="gpu"]').textContent = 'GPU: WebGPU (not available in this browser)';
  } else if (!params.has('engine')) {
    $('engine').value = 'gpu';
  }
  render();
});
