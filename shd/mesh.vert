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

layout(set = 0, binding = 10) uniform sampler2D vat_position;
layout(set = 0, binding = 11) uniform sampler2D vat_rotation;
layout(set = 0, binding = 12) uniform sampler2D vat_lookup;

struct VatInstance
{
    vec4 timing;      // age, fps, frame count, mirrored
    vec4 bounds_min;
    vec4 bounds_max;
};

layout(std430, set = 0, binding = 13) readonly buffer VatInstances
{
    VatInstance vat[];
};

layout(location = 0) out vec3 world_normal;
layout(location = 1) out vec2 uv;
layout(location = 2) out vec3 world_position;
layout(location = 3) flat out uint wound_instance;

layout(push_constant) uniform Draw
{
    mat4 view_projection;
    vec4 base_color;
    vec4 surface;
    vec4 emissive;
    uint joints_per_instance;
    uint wound_enabled;
} pc;

vec2 vat_texture_uv(vec2 source)
{
    return vec2(source.x, 1.0 - source.y);
}

void main()
{
    wound_instance = instance_index;
    Instance instance = instances[instance_index];
    vec3 position = in_position;
    vec3 normal = in_normal;
    mat4 deform = mat4(1.0);

    if (pc.joints_per_instance == 0xffffffffu)
    {
        VatInstance item = vat[instance_index];
        float frames = max(item.timing.z, 1.0);
        float fps = item.timing.y;
        float progress = fract(
            item.timing.x * fps / max(frames - 0.01, 0.01)
        );
        float frame = floor(progress * frames);

        float row_scale = 1.0 - fract(-item.bounds_max.x * 10.0);
        float packed = item.bounds_min.z * 10.0;
        float u_scale = 1.0 - (ceil(packed) - packed);
        float divisor = fract(-item.bounds_min.x * 10.0) >= .5
            ? 2048.0 : 255.0;

        vec2 lookup_uv = vec2(
            in_uv.x * u_scale,
            1.0 - ((1.0 - in_uv.y) * row_scale +
                   (mod(frame, frames) / frames) * row_scale)
        );
        vec4 lookup = textureLod(
            vat_lookup, vat_texture_uv(lookup_uv), 0.0
        );
        vec2 animation_uv = vec2(
            lookup.r + lookup.g / divisor,
            1.0 - (lookup.b + lookup.a / divisor)
        );
        vec4 position_sample = textureLod(
            vat_position, vat_texture_uv(animation_uv), 0.0
        );
        vec4 rotation_sample = textureLod(
            vat_rotation, vat_texture_uv(animation_uv), 0.0
        );

        bool direct_hdr = fract(item.bounds_max.z * 10.0) >= .5;
        position = direct_hdr
            ? position_sample.xyz
            : position_sample.xyz * (
                item.bounds_max.xyz - item.bounds_min.xyz
              ) + item.bounds_min.xyz;

        vec4 quaternion = direct_hdr
            ? rotation_sample
            : rotation_sample * 2.0 - 1.0;

        vec3 up = vec3(0, 1, 0);
        normal = normalize(
            up + 2.0 * cross(
                quaternion.xyz,
                quaternion.w * up + cross(quaternion.xyz, up)
            )
        );

        if (item.timing.w > .5)
        {
            position.z = -position.z;
            normal.z = -normal.z;
        }
        if (in_uv.y <= .1)
        {
            gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
            world_normal = vec3(0, 1, 0);
            world_position = vec3(0);
            uv = in_uv;
            return;
        }
    }
    else if (pc.joints_per_instance != 0u)
    {
        uint first = instance_index * pc.joints_per_instance;
        deform = in_weights.x * bones[first + in_joints.x] +
                 in_weights.y * bones[first + in_joints.y] +
                 in_weights.z * bones[first + in_joints.z] +
                 in_weights.w * bones[first + in_joints.w];
    }

    vec4 world = instance.model * deform * vec4(position, 1.0);
    gl_Position = pc.view_projection * world;
    world_position = world.xyz;
    world_normal = normalize(mat3(instance.normal) * mat3(deform) * normal);
    uv = in_uv;
}
