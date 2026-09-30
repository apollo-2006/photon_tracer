#!/usr/bin/env bash
# Downloads larger test models into models/, from Alec Jacobson's
# common-3d-test-models (https://github.com/alecjacobson/common-3d-test-models).
# The Stanford bunny is already in this repository; these are kept out for
# their size.
#
#   models/fetch.sh armadillo            # by name: armadillo, happy, xyzrgb_dragon
#   models/fetch.sh armadillo happy
#
# The Stanford Bunny, Armadillo, Happy Buddha and Dragon are from the Stanford
# 3D Scanning Repository, http://graphics.stanford.edu/data/3Dscanrep/, which
# asks for credit to the Stanford Computer Graphics Laboratory and allows no
# commercial use without permission.
set -euo pipefail
cd "$(dirname "$0")"
names=("$@")
if [[ ${#names[@]} -eq 0 ]]; then echo "usage: models/fetch.sh NAME... (armadillo, happy, xyzrgb_dragon)"; exit 2; fi
for name in "${names[@]}"; do
  if [[ -f $name.obj ]]; then echo "models/$name.obj is already here"; continue; fi
  echo "fetching models/$name.obj"
  curl -fsSL -o "$name.obj.part" "https://raw.githubusercontent.com/alecjacobson/common-3d-test-models/master/data/$name.obj"
  mv "$name.obj.part" "$name.obj"
done
