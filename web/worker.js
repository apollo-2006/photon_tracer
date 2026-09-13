// One render thread. Loads its own WebAssembly instance, then traces whatever
// row the page hands it and posts the pixels back.
importScripts('photon_tracer.js');

let tracer;
const ready = PhotonTracer().then((m) => { tracer = m; });

onmessage = async ({ data }) => {
  await ready;
  const { j, width, height, spp, bounces } = data;
  const t0 = performance.now();
  const ptr = tracer._trace_row(j, width, height, spp, bounces);
  const pixels = tracer.HEAPU8.slice(ptr, ptr + width * 3);
  postMessage({ j, pixels, rays: tracer._take_rays(), ms: performance.now() - t0 }, [pixels.buffer]);
};
