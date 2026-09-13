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

function render() {
  if (job) stop();
  const width = Number($('res').value), height = Math.round(width * 9 / 16);
  const spp = Number($('spp').value), bounces = Number($('bounces').value);
  const n = Number($('threads').value), bands = $('sched').value === 'bands';
  ensurePool(n);

  const canvas = $('out');
  canvas.width = width; canvas.height = height;
  const ctx = canvas.getContext('2d');
  ctx.fillStyle = '#05080d'; ctx.fillRect(0, 0, width, height);
  const image = ctx.createImageData(width, 1);

  job = { width, height, spp, bounces, next: 0, done: 0, rays: 0, start: performance.now(), perWorker: new Array(n).fill(0), live: true };
  const rowsPerBand = Math.floor(height / n);
  const bandNext = pool.map((_, w) => w * rowsPerBand);
  const bandEnd = pool.map((_, w) => (w === n - 1 ? height : (w + 1) * rowsPerBand));

  // j counts up from the bottom of the image, as in the native renderer.
  const nextFor = (w) => {
    if (bands) return bandNext[w] < bandEnd[w] ? bandNext[w]++ : -1;
    return job.next < height ? job.next++ : -1;
  };

  const bars = $('workers'); bars.textContent = '';
  for (let w = 0; w < n; w++) bars.append(document.createElement('div'));

  pool.forEach((worker, w) => {
    const current = job;
    worker.onmessage = ({ data }) => {
      if (!current.live) return;
      for (let i = 0; i < width; i++) {
        image.data[4 * i] = data.pixels[3 * i];
        image.data[4 * i + 1] = data.pixels[3 * i + 1];
        image.data[4 * i + 2] = data.pixels[3 * i + 2];
        image.data[4 * i + 3] = 255;
      }
      ctx.putImageData(image, 0, height - 1 - data.j);
      current.done++; current.rays += data.rays; current.perWorker[w]++;
      const j = nextFor(w);
      if (j >= 0) worker.postMessage({ j, width, height, spp, bounces });
      update(current);
      if (current.done === height) finish(current);
    };
    const j = nextFor(w);
    if (j >= 0) worker.postMessage({ j, width, height, spp, bounces });
  });
  $('go').disabled = true; $('stop').disabled = false;
  $('schedNote').textContent = bands ? `each worker owns ${rowsPerBand} consecutive rows` : 'workers take whichever row is next';
}

function update(j) {
  const s = (performance.now() - j.start) / 1000;
  $('bar').style.width = (j.done / j.height) * 100 + '%';
  $('sTime').textContent = s.toFixed(2) + ' s';
  $('sRays').textContent = human(j.rays);
  $('sRate').textContent = human(j.rays / Math.max(s, 1e-3));
  $('sRows').textContent = `${j.done}/${j.height}`;
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

$('go').onclick = render;
$('stop').onclick = stop;
render();
