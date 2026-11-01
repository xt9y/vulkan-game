#version 450

layout(location = 0) in vec3 world_normal;
layout(location = 1) in vec2 uv;
layout(location = 2) in vec3 world_position;
layout(location = 0) out vec4 out_albedo;
layout(location = 1) out vec4 out_normal;
layout(location = 2) out vec4 out_material;

layout(set = 0, binding = 4) uniform sampler2D base_texture;
layout(set = 0, binding = 5) uniform sampler2D metallic_roughness_texture;
layout(set = 0, binding = 6) uniform sampler2D normal_texture;
layout(set = 0, binding = 7) uniform sampler2D occlusion_texture;
layout(set = 0, binding = 8) uniform sampler2D emissive_texture;

layout(push_constant) uniform Draw
{
    mat4 view_projection;
    vec4 base_color;
    vec4 surface;
    vec4 emissive;
} pc;

vec3 linear_color(vec3 srgb)
{
    return pow(max(srgb, vec3(0.0)), vec3(2.2));
}

void main()
{
    int flags = int(pc.emissive.w + 0.5);
    vec4 base = pc.base_color;
    if ((flags & 1) != 0)
    {
        vec4 texel = texture(base_texture, uv);
        base *= vec4(linear_color(texel.rgb), texel.a);
    }
    int alpha = (flags >> 5) & 3;
    if (alpha == 1 && base.a < pc.surface.w) discard;

    float metallic = pc.surface.x;
    float roughness = pc.surface.y;
    if ((flags & 2) != 0)
    {
        vec4 mr = texture(metallic_roughness_texture, uv);
        roughness *= mr.g;
        metallic *= mr.b;
    }

    vec3 N = normalize(world_normal);
    if ((flags & 4) != 0)
    {
        vec3 dpdx = dFdx(world_position);
        vec3 dpdy = dFdy(world_position);
        vec2 dtdx = dFdx(uv);
        vec2 dtdy = dFdy(uv);
        float det = dtdx.x * dtdy.y - dtdx.y * dtdy.x;
        if (abs(det) > 1e-7)
        {
            vec3 T = normalize((dpdx * dtdy.y - dpdy * dtdx.y) / det);
            vec3 B = normalize((dpdy * dtdx.x - dpdx * dtdy.x) / det);
            vec3 map_normal = texture(normal_texture, uv).xyz * 2.0 - 1.0;
            N = normalize(mat3(T, B, N) * map_normal);
        }
    }

    float ao = pc.surface.z;
    if ((flags & 8) != 0) ao *= texture(occlusion_texture, uv).r;
    vec3 emission = pc.emissive.rgb;
    if ((flags & 16) != 0)
        emission *= linear_color(texture(emissive_texture, uv).rgb);

    out_albedo = base;
    out_normal = vec4(N * 0.5 + 0.5, 1.0);
    out_material = vec4(
        clamp(roughness, 0.045, 1.0),
        clamp(metallic, 0.0, 1.0),
        clamp(ao, 0.0, 1.0),
        clamp(dot(emission, vec3(0.2126, 0.7152, 0.0722)), 0.0, 1.0)
    );
}
