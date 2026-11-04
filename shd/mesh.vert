#version 450

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in uint instance_index;
layout(location = 4) in uvec4 in_joints;
layout(location = 5) in vec4 in_weights;

struct Instance
{
    mat4 model;
    mat4 normal;
};

layout(std430, set = 0, binding = 0) readonly buffer InstanceBuffer
{
    Instance instances[];
};

layout(std430, set = 0, binding = 9) readonly buffer BonePalette
{
    mat4 bones[];
};

layout(location = 0) out vec3 world_normal;
layout(location = 1) out vec2 uv;
layout(location = 2) out vec3 world_position;
layout(location = 3) flat out uint wound_instance;

layout(push_constant) uniform Draw
{
    mat4 view_projection;
    vec4 base_color;
    vec4 surface;  // metallic, roughness, occlusion, alpha cutoff
    vec4 emissive; // RGB, material flags
    uint joints_per_instance;
    uint wound_enabled;
} pc;

void main()
{
    wound_instance = instance_index;
    Instance instance = instances[instance_index];
    mat4 deform = mat4(1.0);
    if (pc.joints_per_instance != 0u)
    {
        uint first = instance_index * pc.joints_per_instance;
        deform = in_weights.x * bones[first + in_joints.x] +
                 in_weights.y * bones[first + in_joints.y] +
                 in_weights.z * bones[first + in_joints.z] +
                 in_weights.w * bones[first + in_joints.w];
    }
    vec4 world = instance.model * deform * vec4(in_position, 1.0);
    gl_Position = pc.view_projection * world;
    world_position = world.xyz;
    world_normal = normalize(mat3(instance.normal) * mat3(deform) * in_normal);
    uv = in_uv;
}
