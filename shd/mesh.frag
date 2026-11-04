#version 450

layout(location = 0) in vec3 world_normal;
layout(location = 1) in vec2 uv;
layout(location = 2) in vec3 world_position;
layout(location = 3) flat in uint wound_instance;
layout(location = 0) out vec4 out_albedo;
layout(location = 1) out vec4 out_normal;
layout(location = 2) out vec4 out_material;

layout(set = 0, binding = 4) uniform sampler2D base_texture;
layout(set = 0, binding = 5) uniform sampler2D metallic_roughness_texture;
layout(set = 0, binding = 6) uniform sampler2D normal_texture;
layout(set = 0, binding = 7) uniform sampler2D occlusion_texture;
layout(set = 0, binding = 8) uniform sampler2D emissive_texture;

// Per-instance 128 x 128 UV wound atlas, four R8 texels per packed uint.
layout(std430, set = 0, binding = 14) readonly buffer WoundBuffer
{
    uint wound_pixels[];
};

layout(push_constant) uniform Draw
{
    mat4 view_projection;
    vec4 base_color;
    vec4 surface;
    vec4 emissive;
    uint joints_per_instance;
    uint wound_enabled;
} pc;

vec3 linear_color(vec3 srgb)
{
    return pow(max(srgb, vec3(0.0)), vec3(2.2));
}

vec2 wound_texel(ivec2 xy)
{
    xy = clamp(xy, ivec2(0), ivec2(127));
    uint linear = uint(xy.y * 128 + xy.x);
    uint word = wound_pixels[wound_instance * 4096u + linear / 4u];
    uint bits = (word >> ((linear & 3u) * 8u)) & 255u;
    return vec2(float(bits >> 4u), float(bits & 15u)) / 15.0;
}

vec2 wound_sample(vec2 coord)
{
    vec2 p = clamp(coord, vec2(0), vec2(1)) * 127.0;
    ivec2 a = ivec2(floor(p));
    vec2 f = fract(p);
    vec2 aa = wound_texel(a);
    vec2 ba = wound_texel(a + ivec2(1, 0));
    vec2 ab = wound_texel(a + ivec2(0, 1));
    vec2 bb = wound_texel(a + ivec2(1, 1));
    return mix(mix(aa, ba, f.x), mix(ab, bb, f.x), f.y);
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
    // Atlas marks are painted once per actor and follow skinned UVs.
    vec2 wound = pc.wound_enabled != 0u ? wound_sample(uv) : vec2(0);
    float coverage = clamp(wound.x, 0.0, 1.0);
    float wetness = clamp(wound.y, 0.0, 1.0);
    int alpha = (flags >> 5) & 3;
    if (alpha == 1 && base.a < pc.surface.w) discard;
    // Deferred fallback for alpha-blended materials. True transmission
    // requires a separate sorted/weighted forward transparency pass.
    if (alpha == 2)
    {
        const int bayer[16] = int[16](0, 8, 2, 10, 12, 4, 14, 6,
                                      3, 11, 1, 9, 15, 7, 13, 5);
        ivec2 pixel = ivec2(gl_FragCoord.xy) & 3;
        if (base.a <= (float(bayer[pixel.y * 4 + pixel.x]) + 0.5) / 16.0)
            discard;
    }

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

    if (coverage > 0.0)
    {
        const vec3 dry = vec3(.055, .009, .007);
        const vec3 fresh = vec3(.17, .006, .012);
        base.rgb = mix(base.rgb, mix(dry, fresh, wetness), coverage * .94);
        roughness = mix(roughness, mix(.81, .13, wetness), coverage);
        metallic = mix(metallic, 0.0, coverage);
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
