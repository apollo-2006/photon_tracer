// The denoiser's thread. Loads its own instance of the module and runs
// denoise() from include/denoise.hpp over whole frames the page sends it, so
// filtering never holds up the page or the render workers.
importScripts('photon_tracer.js');

const ready = PhotonTracer();

onmessage = async ({ data }) => {
  const m = await ready;
  const { id, width, height, color, albedo, normal, variance } = data;
  const t0 = performance.now();
  // Allocating can grow the module's memory and replace HEAPF32, so the
  // buffers' addresses are read after it.
  m._denoise_buffers(width, height);
  [color, albedo, normal, variance].forEach((a, k) => m.HEAPF32.set(a, m._denoise_buffer(k) >> 2));
  const ptr = m._denoise_run(width, height) >> 2;
  const out = m.HEAPF32.slice(ptr, ptr + 3 * width * height);
  postMessage({ id, out, ms: performance.now() - t0 }, [out.buffer]);
};
