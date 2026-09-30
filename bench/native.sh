#!/usr/bin/env bash
# Times the native renderer on each scene, best of N runs (default 3), with a
# fixed seed so every run traces the same rays. Extra arguments go to every run,
# e.g. bench/native.sh 5 --threads 8. Prints a Markdown table.
set -euo pipefail
cd "$(dirname "$0")/.."
runs=${1:-3}; shift || true
make -s
out=$(mktemp -d); trap 'rm -rf "$out"' EXIT

echo "| scene | best of $runs | rays/s | rays |"
echo "|---|---|---|---|"
for scene in "materials:--no-bvh" "materials, BVH:--bvh" "field:--field" "teapot, 4 spp:--mesh --spp 4" "room, 16 spp:--room --spp 16" "crowd, 400 teapots:--crowd"; do
  name=${scene%%:*}; flags=${scene#*:}
  best=""
  for ((i = 0; i < runs; i++)); do
    # shellcheck disable=SC2086
    line=$(./photon_tracer $flags --seed 1 --out "$out/r.ppm" "$@" 2>&1 | tail -1)
    s=$(sed -E 's/.*rendered in ([0-9.]+) s.*/\1/' <<<"$line")
    if [[ -z $best ]] || awk "BEGIN{exit !($s < $best)}"; then best=$s; bestline=$line; fi
  done
  rate=$(sed -E 's/.* s, ([0-9.]+[kMB]?) rays\/s.*/\1/' <<<"$bestline")
  rays=$(sed -E 's/.*\(([0-9]+) rays\).*/\1/' <<<"$bestline")
  printf '| %s | %.3f s | %s | %s |\n' "$name" "$best" "$rate" "$rays"
done
