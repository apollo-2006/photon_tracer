// One render thread. Loads its own WebAssembly instance, then traces whatever
// row the page hands it and posts the linear pixels back.
importScripts('photon_tracer.js');

let tracer, sceneKey = '';
const ready = PhotonTracer().then((m) => { tracer = m; });

onmessage = async ({ data }) => {
  await ready;
  const { j, width, height, spp, bounces, scene, bvh } = data;
  const key = scene + '/' + bvh;
  if (key !== sceneKey) { tracer._set_scene(scene, bvh ? 1 : 0); sceneKey = key; }
  const t0 = performance.now();
  const ptr = tracer._trace_row(j, width, height, spp, bounces) >> 2;
  const pixels = tracer.HEAPF32.slice(ptr, ptr + width * 3);
  postMessage({ j, spp, pixels, rays: tracer._take_rays(), ms: performance.now() - t0 }, [pixels.buffer]);
};
