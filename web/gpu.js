// The WebGPU renderer: traces a scene the C++ code built and packed
// (include/gpu_pack.hpp, exported by a worker's WebAssembly module) with the
// compute shader in tracer.wgsl, progressively, and shows it on a canvas
// through display.wgsl, optionally through the denoiser in denoise.wgsl.

const HEADER_WORDS = 48;
const SECTIONS = ['nodes', 'order', 'spheres', 'triangles', 'instances', 'materials', 'lights', 'large'];

async function shader(device, url) {
  const module = device.createShaderModule({ code: await (await fetch(url)).text() });
  const info = await module.getCompilationInfo();
  const errors = info.messages.filter((m) => m.type === 'error');
  if (errors.length) throw new Error(`${url}: ${errors.map((m) => `${m.lineNum}:${m.linePos} ${m.message}`).join('; ')}`);
  return module;
}

export class GpuRenderer {
  // A renderer on the best adapter, or null if the browser has no WebGPU.
  static async create() {
    if (!navigator.gpu) return null;
    const adapter = await navigator.gpu.requestAdapter({ powerPreference: 'high-performance' });
    if (!adapter) return null;
    const device = await adapter.requestDevice({
      requiredLimits: { maxStorageBuffersPerShaderStage: 10, maxStorageBufferBindingSize: adapter.limits.maxStorageBufferBindingSize },
    });
    // Validation errors arrive here, not as exceptions.
    device.addEventListener('uncapturederror', (e) => console.error('WebGPU:', e.error.message));
    const r = new GpuRenderer(device, adapter);
    await r.init();
    return r;
  }

  constructor(device, adapter) {
    this.device = device;
    this.adapterInfo = adapter.info || {};
    this.format = navigator.gpu.getPreferredCanvasFormat();
    this.run = 0;  // Incremented by each render() and stop(), so an old loop knows to quit
  }

  async init() {
    const d = this.device;
    const [tracer, display, denoise] = await Promise.all(['tracer.wgsl', 'display.wgsl', 'denoise.wgsl'].map((u) => shader(d, u)));
    // The tracer's workgroup shape, as tracer.wgsl declares it: dispatches
    // must cover the image in those units.
    const wg = (await (await fetch('tracer.wgsl')).text()).match(/@workgroup_size\((\d+),\s*(\d+)\)/);
    this.workgroup = [Number(wg[1]), Number(wg[2])];
    this.tracePipeline = d.createComputePipeline({ layout: 'auto', compute: { module: tracer, entryPoint: 'main' } });
    this.displayPipeline = d.createRenderPipeline({
      layout: 'auto',
      vertex: { module: display, entryPoint: 'vs' },
      fragment: { module: display, entryPoint: 'fs', targets: [{ format: this.format }] },
      primitive: { topology: 'triangle-list' },
    });
    this.denoisePipelines = Object.fromEntries(['prepare', 'atrous', 'finish'].map((e) => [
      e, d.createComputePipeline({ layout: 'auto', compute: { module: denoise, entryPoint: e } })]));
  }

  // Uploads a packed scene (an ArrayBuffer from gpu_scene()).
  loadScene(packed) {
    const d = this.device;
    const words = new Uint32Array(packed, 0, HEADER_WORDS);
    if (words[0] !== 0x50475450) throw new Error('not a packed scene');
    for (const b of Object.values(this.sceneBuffers || {})) b.destroy();
    this.sceneBuffers = {};
    SECTIONS.forEach((name, k) => {
      const offset = words[2 + 2 * k], length = words[3 + 2 * k];
      // A binding must hold at least one element of its array, even for a
      // scene with none (the largest element, a BVH node, is 128 bytes).
      const size = Math.max(128, Math.ceil(length / 16) * 16);
      const buf = d.createBuffer({ size, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST });
      if (length) d.queue.writeBuffer(buf, 0, packed, offset, Math.ceil(length / 4) * 4);
      this.sceneBuffers[name] = buf;
    });
    // Lights and large spheres share one binding: lights first.
    const lights = new Uint32Array(packed, words[2 + 12], words[3 + 12] / 4);
    const large = new Uint32Array(packed, words[2 + 14], words[3 + 14] / 4);
    const lists = new Uint32Array(Math.max(4, lights.length + large.length));
    lists.set(lights);
    lists.set(large, lights.length);
    this.sceneBuffers.lists = d.createBuffer({ size: lists.byteLength, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST });
    d.queue.writeBuffer(this.sceneBuffers.lists, 0, lists);
    this.scene = {
      flatRoot: words[18], instanceRoot: words[19], smallSpheres: words[20], flags: words[21],
      camera: new Float32Array(packed, 4 * 22, 16).slice(), lights: lights.length, large: large.length,
    };
  }

  // Sizes the per-pixel buffers and binds everything for width x height.
  setup(width, height) {
    const d = this.device;
    if (this.size && this.size[0] === width && this.size[1] === height && this.boundScene === this.sceneBuffers) return;
    for (const b of this.pixelBuffers || []) b.destroy();
    const px = width * height;
    const storage = (bytes) => d.createBuffer({ size: bytes, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST | GPUBufferUsage.COPY_SRC });
    this.accumulated = storage(16 * px);
    this.guides = storage(32 * px);
    this.surface = storage(32 * px);
    this.lightA = storage(16 * px);
    this.lightB = storage(16 * px);
    this.denoised = storage(16 * px);
    this.stats = storage(16);
    this.frameUniform = d.createBuffer({ size: 112, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
    this.viewUniform = d.createBuffer({ size: 16, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
    this.pixelBuffers = [this.accumulated, this.guides, this.surface, this.lightA, this.lightB, this.denoised, this.stats,
                         this.frameUniform, this.viewUniform];

    const s = this.sceneBuffers;
    const entries = (list) => list.map((resource, binding) => ({ binding, resource: { buffer: resource } }));
    this.traceGroup = d.createBindGroup({
      layout: this.tracePipeline.getBindGroupLayout(0),
      entries: entries([this.frameUniform, s.nodes, s.order, s.spheres, s.triangles, s.instances, s.materials, s.lists,
                        this.accumulated, this.guides, this.stats]),
    });
    this.displayGroup = d.createBindGroup({
      layout: this.displayPipeline.getBindGroupLayout(0),
      entries: entries([this.viewUniform, this.accumulated, this.denoised]),
    });
    // The denoiser: prepare writes lightA, five a-trous passes alternate
    // A -> B -> A ..., ending in B, and finish reads B.
    this.denoiseSteps = [];
    const params = (stride) => {
      const u = d.createBuffer({ size: 16, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
      d.queue.writeBuffer(u, 0, new Uint32Array([width, height, stride, 0]));
      this.pixelBuffers.push(u);
      return u;
    };
    const group = (pipeline, uniform, src, dst) => d.createBindGroup({
      layout: pipeline.getBindGroupLayout(0),
      entries: entries([uniform, this.accumulated, this.guides, this.surface, src, dst, this.denoised])
        .filter((e) => pipeline !== this.denoisePipelines.atrous || ![1, 2, 6].includes(e.binding))
        .filter((e) => pipeline !== this.denoisePipelines.prepare || e.binding !== 5 && e.binding !== 6)
        .filter((e) => pipeline !== this.denoisePipelines.finish || ![1, 2, 5].includes(e.binding)),
    });
    const p = this.denoisePipelines;
    this.denoiseSteps.push([p.prepare, group(p.prepare, params(0), this.lightA, this.lightB)]);
    for (let k = 0; k < 5; k++) {
      const [src, dst] = k % 2 === 0 ? [this.lightA, this.lightB] : [this.lightB, this.lightA];
      this.denoiseSteps.push([p.atrous, group(p.atrous, params(1 << k), src, dst)]);
    }
    this.denoiseSteps.push([p.finish, group(p.finish, params(0), this.lightB, this.lightA)]);
    this.size = [width, height];
    this.boundScene = this.sceneBuffers;
  }

  writeFrame(width, height, bounces, guides, seed, firstSample, samples) {
    const c = this.scene.camera;
    const buf = new ArrayBuffer(112);
    new Float32Array(buf, 0, 16).set(c);
    new Uint32Array(buf, 64, 12).set([
      width, height, bounces, this.scene.flags | (guides ? 4 : 0),
      this.scene.flatRoot, this.scene.instanceRoot, this.scene.smallSpheres, seed,
      this.scene.lights, this.scene.large, firstSample, samples,
    ]);
    this.device.queue.writeBuffer(this.frameUniform, 0, buf);
  }

  encodeTrace(encoder, width, height) {
    const pass = encoder.beginComputePass();
    pass.setPipeline(this.tracePipeline);
    pass.setBindGroup(0, this.traceGroup);
    pass.dispatchWorkgroups(Math.ceil(width / this.workgroup[0]), Math.ceil(height / this.workgroup[1]));
    pass.end();
  }

  encodeDenoise(encoder, width, height) {
    for (const [pipeline, group] of this.denoiseSteps) {
      const pass = encoder.beginComputePass();
      pass.setPipeline(pipeline);
      pass.setBindGroup(0, group);
      pass.dispatchWorkgroups(Math.ceil(width / 8), Math.ceil(height / 8));
      pass.end();
    }
  }

  draw(context, width, height, denoised) {
    this.device.queue.writeBuffer(this.viewUniform, 0, new Uint32Array([width, height, denoised ? 1 : 0, 0]));
    const encoder = this.device.createCommandEncoder();
    const pass = encoder.beginRenderPass({
      colorAttachments: [{ view: context.getCurrentTexture().createView(), loadOp: 'clear', storeOp: 'store', clearValue: [0, 0, 0, 1] }],
    });
    pass.setPipeline(this.displayPipeline);
    pass.setBindGroup(0, this.displayGroup);
    pass.draw(3);
    pass.end();
    this.device.queue.submit([encoder.finish()]);
  }

  async readStats() {
    const staging = this.device.createBuffer({ size: 16, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
    const encoder = this.device.createCommandEncoder();
    encoder.copyBufferToBuffer(this.stats, 0, staging, 0, 16);
    this.device.queue.submit([encoder.finish()]);
    await staging.mapAsync(GPUMapMode.READ);
    const w = new Uint32Array(staging.getMappedRange()).slice();
    staging.destroy();
    return w[0] + w[1] * 2 ** 32;
  }

  // Renders spp samples per pixel into the accumulation buffer, a batch per
  // step, and calls onStep(samplesDone) after each. Batches start at one
  // sample and grow while a step stays short, so the first noisy frame comes
  // at once and later ones do not hold the page up. Each step waits for the
  // GPU before the next, via next(), which the page passes as
  // requestAnimationFrame when it shows progress.
  async render({ width, height, spp, bounces, denoise, seed, onStep, next }) {
    const run = ++this.run;
    this.setup(width, height);
    const d = this.device;
    const clear = d.createCommandEncoder();
    for (const b of [this.accumulated, this.guides, this.stats]) clear.clearBuffer(b);
    d.queue.submit([clear.finish()]);
    let done = 0, batch = 1;
    while (done < spp) {
      if (run !== this.run) return false;
      const n = Math.min(batch, spp - done);
      this.writeFrame(width, height, bounces, denoise, seed >>> 0, done, n);
      const encoder = d.createCommandEncoder();
      this.encodeTrace(encoder, width, height);
      if (denoise) this.encodeDenoise(encoder, width, height);
      const t0 = performance.now();
      d.queue.submit([encoder.finish()]);
      await d.queue.onSubmittedWorkDone();
      const ms = performance.now() - t0;
      done += n;
      if (ms < 20 && batch < 64) batch *= 2;
      else if (ms > 60 && batch > 1) batch = Math.max(1, batch >> 1);
      if (onStep) onStep(done);
      if (next && done < spp) await new Promise((r) => next(r));
    }
    return run === this.run;
  }

  stop() { this.run++; }

  // The accumulated image as linear RGB floats, top row first.
  async readImage(width, height) {
    const bytes = 16 * width * height;
    const staging = this.device.createBuffer({ size: bytes, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
    const encoder = this.device.createCommandEncoder();
    encoder.copyBufferToBuffer(this.accumulated, 0, staging, 0, bytes);
    this.device.queue.submit([encoder.finish()]);
    await staging.mapAsync(GPUMapMode.READ);
    const acc = new Float32Array(staging.getMappedRange());
    const out = new Float32Array(3 * width * height);
    for (let p = 0; p < width * height; p++) {
      const n = acc[4 * p + 3] || 1;
      for (let c = 0; c < 3; c++) out[3 * p + c] = acc[4 * p + c] / n;
    }
    staging.destroy();
    return out;
  }
}
