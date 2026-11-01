#version 450

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in uint instance_index;

struct Instance
{
    mat4 model;
    mat4 normal;
};

layout(std430, set = 0, binding = 0) readonly buffer InstanceBuffer
{
    Instance instances[];
};

layout(location = 0) out vec3 world_normal;
layout(location = 1) out vec2 uv;
layout(location = 2) out vec3 world_position;

layout(push_constant) uniform Draw
{
    mat4 view_projection;
    vec4 base_color;
    vec4 surface;  // metallic, roughness, occlusion, alpha cutoff
    vec4 emissive; // RGB, alpha mode
} pc;

void main()
{
    Instance instance = instances[instance_index];
    vec4 world = instance.model * vec4(in_position, 1.0);
    gl_Position = pc.view_projection * world;
    world_position = world.xyz;
    world_normal = normalize(mat3(instance.normal) * in_normal);
    uv = in_uv;
}
