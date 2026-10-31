#version 450

layout(location = 0) in vec3 world_normal;
layout(location = 0) out vec4 out_color;

void main()
{
    vec3 normal = normalize(world_normal);
    vec3 light_direction = normalize(vec3(0.5, 1.0, 0.3));

    float diffuse = max(dot(normal, light_direction), 0.0);

    vec3 albedo = vec3(0.65, 0.72, 0.82);
    vec3 color = albedo * (0.25 + 0.75 * diffuse);

    out_color = vec4(color, 1.0);
}
