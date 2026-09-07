#version 460 core
#extension GL_ARB_shader_draw_parameters : require

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;

layout(std140, binding = 0) uniform CameraBlock {
    mat4 viewProjection;
    vec4 frustumPlanes[6];
    vec4 viewportAndNearFar;
    uint objectCount;
} camera;

struct ObjectGpu { mat4 world; vec4 sphere; uvec4 identity; };
struct VisibleRecord { uint objectIndex; uint lod; uint generation; float projectedPixels; };
layout(std430, binding = 1) readonly buffer Objects { ObjectGpu objectData[]; };
layout(std430, binding = 2) readonly buffer Visible { VisibleRecord visible[]; };

out gl_PerVertex { vec4 gl_Position; };
layout(location = 0) out vec3 worldNormal;
layout(location = 1) flat out uint materialIndex;
layout(location = 2) flat out uint selectedLod;

void main() {
    VisibleRecord record = visible[gl_BaseInstance + gl_InstanceID];
    ObjectGpu object = objectData[record.objectIndex];
    vec4 worldPosition = object.world * vec4(inPosition, 1.0);
    worldNormal = normalize(mat3(object.world) * inNormal);
    materialIndex = object.identity.w;
    selectedLod = record.lod;
    gl_Position = camera.viewProjection * worldPosition;
}
