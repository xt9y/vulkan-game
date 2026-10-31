#version 450

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in mat4 instance_model;
layout(location = 6) in mat4 instance_normal;

layout(location = 0) out vec3 world_normal;

layout(push_constant) uniform Draw
{
    mat4 view_projection;
} pc;

void main()
{
    gl_Position = pc.view_projection * instance_model * vec4(in_position, 1.0);
    world_normal = normalize(mat3(instance_normal) * in_normal);
}
