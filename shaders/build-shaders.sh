#!/usr/bin/env bash
#
# Compiles the engine's GLSL shaders to SPIR-V and writes them as C headers
# under include/lotus/gpu/. The generated headers are committed, so a normal
# build never needs a shader compiler.
#
# Requires glslangValidator (Fedora: `dnf install glslang`, or point GLSLANG
# at a copy extracted from the RPM when you cannot install packages):
#
#   GLSLANG=/tmp/shadertools/usr/bin/glslangValidator ./shaders/build-shaders.sh
#
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$(dirname "$here")"
out_dir="$root/include/lotus/gpu"
glslang="${GLSLANG:-glslangValidator}"

if ! command -v "$glslang" >/dev/null 2>&1; then
    echo "error: glslangValidator not found (set GLSLANG=/path/to/glslangValidator)" >&2
    exit 1
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# "<stage> <base>": lit supplies vert and frag, depth supplies comp.
for pair in "vert lit" "frag lit" "comp depth"; do
    stage="${pair%% *}"
    base="${pair##* }"
    source="$here/$base.$stage"
    spv="$work/$base.$stage.spv"
    header="$out_dir/$base.$stage.spv.h"
    name="${base}_${stage}_spv"

    "$glslang" -V "$source" -o "$spv" >/dev/null

    python3 - "$spv" "$name" "$header" "$base.$stage" <<'PY'
import sys

spv_path, name, header_path, source_name = sys.argv[1:5]
data = open(spv_path, "rb").read()

lines = [
    "#pragma once",
    f"// Generated from shaders/{source_name} by shaders/build-shaders.sh. Do not edit.",
    f"static const unsigned char {name}[] = {{",
]
for i in range(0, len(data), 12):
    chunk = ", ".join(f"0x{b:02x}" for b in data[i:i + 12])
    lines.append(f"  {chunk},")
lines.append("};")
lines.append(f"static const unsigned int {name}_len = {len(data)};")
lines.append("")

open(header_path, "w").write("\n".join(lines))
PY

    echo "wrote $header"
done