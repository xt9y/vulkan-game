#version 450
layout(location = 0) in vec3 world_normal;
layout(location = 0) out vec4 out_albedo;
layout(location = 1) out vec4 out_normal;
layout(location = 2) out vec4 out_material;

void main()
{
    vec3 normal = normalize(world_normal);
    out_albedo = vec4(0.65, 0.72, 0.82, 1.0);
    out_normal = vec4(normal * 0.5 + 0.5, 1.0);
    out_material = vec4(0.5, 0.0, 0.0, 1.0);
}
