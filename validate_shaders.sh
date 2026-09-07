#!/usr/bin/env sh
set -eu

if ! command -v glslangValidator >/dev/null 2>&1; then
    echo "glslangValidator is required to validate GLSL" >&2
    exit 2
fi

for shader in shaders/*.comp; do
    glslangValidator --target-env opengl -S comp "$shader"
done
for shader in shaders/*.vert; do
    glslangValidator --target-env opengl -S vert "$shader"
done
for shader in shaders/*.frag; do
    glslangValidator --target-env opengl -S frag "$shader"
done
