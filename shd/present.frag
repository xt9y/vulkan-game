#version 450
layout(location=0) in vec2 uv;
layout(set=0,binding=0) uniform sampler2D source_image;
layout(location=0) out vec4 color;

layout(push_constant) uniform Options
{
    int visible, selected, lighting, tonemapping;
    float exposure, fov, sensitivity, padding;
} ui;

const uvec2 captions[6] = uvec2[6](
    uvec2(0x4f505449u, 0x4f4e5300u),
    uvec2(0x4c494748u, 0x54000000u),
    uvec2(0x544f4e45u, 0x00000000u),
    uvec2(0x4558504fu, 0x53450000u),
    uvec2(0x464f5600u, 0x00000000u),
    uvec2(0x53454e53u, 0x00000000u)
);

uint glyph(uint letter)
{
    switch (letter)
    {
        case 67u: return 0x724Fu;
        case 69u: return 0x72CFu;
        case 70u: return 0x12CFu;
        case 71u: return 0x7B4Fu;
        case 72u: return 0x5BEDu;
        case 73u: return 0x7497u;
        case 76u: return 0x7249u;
        case 78u: return 0x5FFDu;
        case 79u: return 0x7B6Fu;
        case 80u: return 0x13EFu;
        case 83u: return 0x79CFu;
        case 84u: return 0x2497u;
        case 85u: return 0x7B6Du;
        case 86u: return 0x2B6Du;
        case 88u: return 0x5AADu;
        default: return 0u;
    }
}

bool text_pixel(ivec2 p, int line)
{
    int top = line == 0 ? 34 : 64 + (line - 1) * 30;
    ivec2 glyph_pos = p - ivec2(35, top);
    if (glyph_pos.x < 0 || glyph_pos.y < 0 || glyph_pos.y >= 15) return false;

    int character = glyph_pos.x / 12;
    if (character >= 8) return false;
    int x = (glyph_pos.x % 12) / 3;
    int y = glyph_pos.y / 3;
    if (x >= 3) return false;

    uint bits = character < 4 ? captions[line].x : captions[line].y;
    uint letter = (bits >> uint((3 - character % 4) * 8)) & 255u;
    return (glyph(letter) & (1u << uint(y * 3 + x))) != 0u;
}

void main()
{
    vec3 scene = texture(source_image, uv).rgb;
    if (ui.visible == 0)
    {
        color = vec4(scene, 1.0);
        return;
    }

    ivec2 p = ivec2(gl_FragCoord.xy);
    p.y = textureSize(source_image, 0).y - 1 - p.y;
    if (p.x < 20 || p.x >= 305 || p.y < 20 || p.y >= 230)
    {
        color = vec4(scene, 1.0);
        return;
    }

    vec3 result = mix(scene, vec3(0.025, 0.031, 0.044), 0.94);
    int row = (p.y - 60) / 30;
    bool in_row = p.y >= 60 && row >= 0 && row < 5;
    if (in_row && row == ui.selected)
        result = vec3(0.11, 0.16, 0.22);

    if (in_row && p.x >= 159 && p.x < 280 &&
        p.y >= 71 + row * 30 && p.y < 80 + row * 30)
    {
        float fill = row == 0 ? float(ui.lighting) :
                     row == 1 ? float(ui.tonemapping) :
                     row == 2 ? ui.exposure :
                     row == 3 ? ui.fov : ui.sensitivity;
        result = float(p.x - 159) / 121.0 < clamp(fill, 0.0, 1.0) ?
                 vec3(0.75, 0.82, 0.9) : vec3(0.23, 0.27, 0.32);
    }

    for (int line = 0; line < 6; ++line)
        if (text_pixel(p, line))
            result = vec3(0.94, 0.96, 0.98);

    color = vec4(result, 1.0);
}
