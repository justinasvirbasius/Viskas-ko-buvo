#version 460 core

layout(location = 0) in vec3 worldNormal;
layout(location = 1) flat in uint materialIndex;
layout(location = 2) flat in uint selectedLod;
layout(location = 0) out vec4 outColor;

vec3 palette(uint id) {
    float x = float(id % 17u) / 17.0;
    return 0.48 + 0.42 * cos(6.28318 * (x + vec3(0.0, 0.31, 0.67)));
}

void main() {
    vec3 n = normalize(worldNormal);
    vec3 light = normalize(vec3(0.35, 0.8, 0.45));
    float diffuse = max(dot(n, light), 0.0);
    float coatSignal = 1.0 - float(selectedLod) * 0.12;
    vec3 color = palette(materialIndex) * (0.12 + 0.88 * diffuse) * coatSignal;
    outColor = vec4(color, 1.0);
}
