#!/usr/bin/env bash
# Downloads Intel's XeSS library (libxess.dll on Windows) from the official XeSS SDK repository (github.com/intel/xess).
# bbport loads it through the Vulkan API when upscaler=xess: it is searched next
# to the executable (out/ by default) and in BB_XESS_DIR. Works on all modern GPUs (Intel, AMD, NVIDIA).
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
dest=${1:-out}
mkdir -p "$dest"
name="libxess.dll"
if [[ -s $dest/$name ]]; then
    echo "XeSS: $dest/$name present"
    exit 0
fi
echo "XeSS: downloading $dest/$name from github.com/intel/xess..."
curl -fsSL --retry 3 -o "$dest/$name.part" "https://raw.githubusercontent.com/intel/xess/main/bin/libxess.dll"
mv "$dest/$name.part" "$dest/$name"
echo "XeSS: downloaded $dest/$name"
