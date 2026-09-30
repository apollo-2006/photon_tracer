#!/usr/bin/env bash
# Downloads larger test models into models/, from Alec Jacobson's
# common-3d-test-models (https://github.com/alecjacobson/common-3d-test-models).
# They are not in this repository: the Stanford scans are free to use with
# acknowledgement but not to redistribute under this project's MIT license.
#
#   models/fetch.sh             # the Stanford bunny (69,451 triangles, 2.4 MB)
#   models/fetch.sh armadillo   # others by name, e.g. armadillo, happy, xyzrgb_dragon
#
# The Stanford Bunny, Armadillo, Happy Buddha and Dragon are from the Stanford
# 3D Scanning Repository, http://graphics.stanford.edu/data/3Dscanrep/.
set -euo pipefail
cd "$(dirname "$0")"
names=("$@")
[[ ${#names[@]} -eq 0 ]] && names=(stanford-bunny)
for name in "${names[@]}"; do
  if [[ -f $name.obj ]]; then echo "models/$name.obj is already here"; continue; fi
  echo "fetching models/$name.obj"
  curl -fsSL -o "$name.obj.part" "https://raw.githubusercontent.com/alecjacobson/common-3d-test-models/master/data/$name.obj"
  mv "$name.obj.part" "$name.obj"
done
