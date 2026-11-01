#version 450

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in uint instance_index;

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

layout(push_constant) uniform Draw
{
    mat4 view_projection;
} pc;

void main()
{
    Instance instance = instances[instance_index];
    gl_Position = pc.view_projection * instance.model * vec4(in_position, 1.0);
    world_normal = normalize(mat3(instance.normal) * in_normal);
}
