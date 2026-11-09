#version 450
// Captures only visible glass and reflective cube faces. Opaque geometry
// remains in the ordinary G-buffer and is available for transmission.
layout(location=0) in vec3 world_normal;
layout(location=1) in vec2 uv;
layout(location=2) in vec3 world_position;
layout(location=3) flat in vec4 instance_tint;
layout(location=0) out vec4 out_normal;
layout(location=1) out vec4 out_tint;
layout(location=2) out vec4 out_material;

void main()
{
    float kind=floor(instance_tint.w+0.000001);
    if(kind!=2.0 && kind!=3.0)discard;
    uint packed=uint(round(fract(instance_tint.w)*65536.0));
    float strength=float((packed>>8u)&255u)/255.0;
    float mattness=float(packed&255u)/255.0;
    vec3 N=normalize(world_normal);
    out_normal=vec4(N*.5+.5,kind==2.0?.5:1.0);
    out_tint=vec4(instance_tint.rgb,strength);
    out_material=vec4(mattness,kind==2.0?1.52:1.0,0.0,1.0);
}
