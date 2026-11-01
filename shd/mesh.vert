#version 450

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;

layout(location = 0) out vec3 world_normal;

layout(push_constant) uniform Draw
{
    mat4 mvp;
    mat4 normal_matrix;
} pc;

void main()
{
    gl_Position = pc.mvp * vec4(in_position, 1.0);
    world_normal = normalize(
        mat3(pc.normal_matrix) * in_normal
    );
}
