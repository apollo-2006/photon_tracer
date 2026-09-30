// One render thread. Loads its own WebAssembly instance, then traces whatever
// row the page hands it and posts the linear pixels back.
importScripts('photon_tracer.js');

let tracer, sceneKey = '', objLoaded = false, seed = -1;
const ready = PhotonTracer().then((m) => { tracer = m; });

// The page keeps two rows in flight, so a second message can arrive while the
// first is still awaiting the module or the teapot. Chaining handles them in
// order, so the scene is only ever set up once.
let queue = ready;
onmessage = ({ data }) => { queue = queue.then(() => trace(data)); };

async function trace(data) {
  const { j, pass, width, height, spp, first, bounces, scene, bvh, aux } = data;
  if (data.seed !== seed) { tracer._set_seed(data.seed); seed = data.seed; }
  const key = scene + '/' + bvh;
  if (key !== sceneKey) {
    // The mesh and crowd scenes need the model in the module's memory first. HEAPU8 is
    // read after obj_buffer() because allocating can grow and replace it.
    if ((scene === 2 || scene === 4) && !objLoaded) {
      const bytes = new Uint8Array(await (await fetch('teapot.obj')).arrayBuffer());
      tracer.HEAPU8.set(bytes, tracer._obj_buffer(bytes.length));
      objLoaded = true;
    }
    tracer._set_scene(scene, bvh ? 1 : 0);
    sceneKey = key;
  }
  const t0 = performance.now();
  const ptr = tracer._trace_row(j, width, height, spp, bounces, first, aux ? 1 : 0) >> 2;
  const pixels = tracer.HEAPF32.slice(ptr, ptr + width * 3);
  // The denoiser's guide data for the row, when the page is denoising.
  let guide = null;
  if (aux) { const a = tracer._row_aux() >> 2; guide = tracer.HEAPF32.slice(a, a + width * 7); }
  const transfer = guide ? [pixels.buffer, guide.buffer] : [pixels.buffer];
  postMessage({ j, pass, spp, pixels, guide, rays: tracer._take_rays(), ms: performance.now() - t0 }, transfer);
}
