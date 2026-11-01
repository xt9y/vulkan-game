#version 450

layout(location = 0) in vec3 world_normal;
layout(location = 1) in vec2 uv;
layout(location = 0) out vec4 out_albedo;
layout(location = 1) out vec4 out_normal;
layout(location = 2) out vec4 out_material;

layout(push_constant) uniform Draw
{
    mat4 view_projection;
    vec4 base_color;
    vec4 surface;
    vec4 emissive;
} pc;

void main()
{
    vec4 base = pc.base_color;
    if (pc.emissive.w > 0.5 && pc.emissive.w < 1.5 &&
        base.a < pc.surface.w) discard;

    vec3 normal = normalize(world_normal);
    out_albedo = vec4(base.rgb, base.a);
    out_normal = vec4(normal * 0.5 + 0.5, 1.0);
    out_material = vec4(
        clamp(pc.surface.y, 0.045, 1.0),
        clamp(pc.surface.x, 0.0, 1.0),
        clamp(pc.surface.z, 0.0, 1.0),
        clamp(dot(pc.emissive.rgb, vec3(0.2126, 0.7152, 0.0722)), 0.0, 1.0)
    );
}
