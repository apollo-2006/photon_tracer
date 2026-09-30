#!/usr/bin/env python3
"""Render regression tests for photon_tracer.

A path traced image is noisy, so a new render never matches a reference byte
for byte, and the noise differs from pixel to pixel: sky is exact, glass is
not. Instead, each scene has a reference (tests/reference.json) holding, for
every BLOCK x BLOCK block of the image, the mean linear color and how much
that mean varies between renders with different seeds, measured from SEEDS
renders. A check renders the scene once with a seed the reference did not use
and asks whether each block, each row of blocks, and the whole image lie
within what that noise explains. A change that moves the image, such as a
precision bug darkening the ground, fails; a different noise pattern passes.

Renders are compared in linear color (PFM output) rather than as display
bytes, because averaging after gamma and clamping biases noisy pixels.

    tests/render_test.py check        # what CI runs
    tests/render_test.py reference    # rewrite tests/reference.json after an intended change

Standard library only, so CI needs nothing installed.
"""
import argparse
import array
import json
import math
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REFERENCE = os.path.join(ROOT, "tests", "reference.json")
BINARY = os.path.join(ROOT, "photon_tracer")

WIDTH = 960       # 960 x 540
SPP = 200
BLOCK = 20        # 48 x 27 blocks
SEEDS = 16        # renders behind each reference
CHECK_SEED = 1    # not one of the reference seeds, which start at 1000

# Flags per scene, run exactly as a user would.
SCENES = {
    "materials": [],
    "field": ["--field"],
    "teapot": ["--mesh"],
    "room": ["--room"],
}

# Largest |z| allowed for a single block, for a row of blocks, and for the whole
# image. The block limit is high because there are thousands of blocks and each
# sigma is itself estimated from SEEDS renders, which gives heavier tails than
# a normal distribution; the row and image limits are where a small, broad
# shift shows up, since it adds up over many blocks.
Z_BLOCK, Z_ROW, Z_IMAGE = 8.0, 6.0, 5.0

# Blocks of pure sky vary by almost nothing between seeds, and a different
# compiler rounds differently. A floor on sigma keeps those blocks from
# failing on rounding.
SIGMA_FLOOR = 2e-4


def render(scene, seed, path, extra=()):
    cmd = [BINARY, *SCENES[scene], "--width", str(WIDTH), "--spp", str(SPP),
           "--seed", str(seed), "--out", path, *extra]
    subprocess.run(cmd, cwd=ROOT, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def read_pfm(path):
    """Returns (width, height, floats), rows top first."""
    with open(path, "rb") as f:
        assert f.readline().strip() == b"PF", "not a color PFM"
        w, h = map(int, f.readline().split())
        scale = float(f.readline())
        data = array.array("f")
        data.frombytes(f.read())
    if (scale < 0) != (sys.byteorder == "little"):
        data.byteswap()
    assert len(data) == 3 * w * h
    rows = [data[3 * w * y: 3 * w * (y + 1)] for y in range(h)]
    rows.reverse()  # PFM stores the bottom row first
    return w, h, rows


def block_means(path):
    """Mean linear color of each block, as a flat list [r, g, b, r, g, b, ...]
    with blocks in row-major order."""
    w, h, rows = read_pfm(path)
    bw, bh = w // BLOCK, h // BLOCK
    sums = [0.0] * (3 * bw * bh)
    for y in range(bh * BLOCK):
        row, base = rows[y], 3 * bw * (y // BLOCK)
        for bx in range(bw):
            start = 3 * BLOCK * bx
            for c in range(3):
                sums[base + 3 * bx + c] += sum(row[start + c: start + 3 * BLOCK: 3])
    n = BLOCK * BLOCK
    return bw, bh, [s / n for s in sums]


def make_reference(args):
    ref = {"width": WIDTH, "spp": SPP, "block": BLOCK, "seeds": SEEDS, "scenes": {}}
    with tempfile.TemporaryDirectory() as tmp:
        for scene in args.scenes:
            runs = []
            for k in range(SEEDS):
                path = os.path.join(tmp, f"{scene}.pfm")
                render(scene, 1000 + k, path)
                bw, bh, means = block_means(path)
                runs.append(means)
                print(f"\r{scene}: {k + 1}/{SEEDS} renders", end="", flush=True)
            print()
            mean = [sum(v) / SEEDS for v in zip(*runs)]
            sigma = [math.sqrt(sum((x - m) ** 2 for x in v) / (SEEDS - 1)) for v, m in zip(zip(*runs), mean)]
            ref["scenes"][scene] = {
                "blocks": [bw, bh],
                "mean": [float(f"{x:.6g}") for x in mean],
                "sigma": [float(f"{x:.4g}") for x in sigma],
            }
    if os.path.exists(REFERENCE):
        with open(REFERENCE) as f:
            old = json.load(f)
        # Keep scenes not rebuilt this time.
        for scene, data in old.get("scenes", {}).items():
            ref["scenes"].setdefault(scene, data)
    with open(REFERENCE, "w") as f:
        json.dump(ref, f, separators=(",", ":"))
        f.write("\n")
    print(f"wrote {os.path.relpath(REFERENCE, ROOT)}")


def check_scene(scene, data, tmp, seed):
    path = os.path.join(tmp, f"{scene}.pfm")
    render(scene, seed, path)
    bw, bh, means = block_means(path)
    assert [bw, bh] == data["blocks"], "block grid differs from the reference"
    # The reference mean is itself an average of SEEDS renders, so it carries
    # a little of the same noise.
    scale = math.sqrt(1 + 1 / SEEDS)
    diffs, variances = [], []
    for m, r, s in zip(means, data["mean"], data["sigma"]):
        diffs.append(m - r)
        variances.append((max(s, SIGMA_FLOOR) * scale) ** 2)

    worst_block = max(abs(d) / math.sqrt(v) for d, v in zip(diffs, variances))
    # Sums over a row of blocks, and over everything, per channel.
    worst_row = 0.0
    for by in range(bh):
        for c in range(3):
            idx = range(3 * bw * by + c, 3 * bw * (by + 1), 3)
            z = sum(diffs[i] for i in idx) / math.sqrt(sum(variances[i] for i in idx))
            worst_row = max(worst_row, abs(z))
    image_z = []
    for c in range(3):
        z = sum(diffs[c::3]) / math.sqrt(sum(variances[c::3]))
        image_z.append(z)
    worst_image = max(abs(z) for z in image_z)

    ok = worst_block <= Z_BLOCK and worst_row <= Z_ROW and worst_image <= Z_IMAGE
    print(f"{'ok  ' if ok else 'FAIL'} {scene:10s} worst block z {worst_block:5.2f} (<= {Z_BLOCK}), "
          f"row z {worst_row:5.2f} (<= {Z_ROW}), image z "
          f"{', '.join(f'{z:+.2f}' for z in image_z)} (|z| <= {Z_IMAGE})")
    return ok


def check_seed_is_reproducible(tmp):
    """The same seed must give the same bytes on one thread and on several."""
    paths = []
    for threads in (1, 3):
        path = os.path.join(tmp, f"seed{threads}.ppm")
        subprocess.run([BINARY, "--field", "--width", "320", "--spp", "8", "--seed", "7",
                        "--threads", str(threads), "--out", path],
                       cwd=ROOT, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        with open(path, "rb") as f:
            paths.append(f.read())
    ok = paths[0] == paths[1]
    print(f"{'ok  ' if ok else 'FAIL'} same seed, 1 and 3 threads: {'identical' if ok else 'different'} images")
    return ok


def check(args):
    with open(REFERENCE) as f:
        ref = json.load(f)
    if (ref["width"], ref["spp"], ref["block"]) != (WIDTH, SPP, BLOCK):
        sys.exit("tests/reference.json was made with other settings; run: tests/render_test.py reference")
    ok = True
    with tempfile.TemporaryDirectory() as tmp:
        ok &= check_seed_is_reproducible(tmp)
        for scene in args.scenes:
            ok &= check_scene(scene, ref["scenes"][scene], tmp, args.seed)
    sys.exit(0 if ok else 1)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("command", choices=["check", "reference"])
    parser.add_argument("--scene", action="append", dest="scenes", choices=list(SCENES),
                        help="only this scene (repeatable); default all")
    parser.add_argument("--seed", type=int, default=CHECK_SEED, help="seed for check renders")
    args = parser.parse_args()
    args.scenes = args.scenes or list(SCENES)
    if not os.path.exists(BINARY):
        sys.exit("build first: make")
    (make_reference if args.command == "reference" else check)(args)


if __name__ == "__main__":
    main()
