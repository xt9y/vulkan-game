#pragma once
#include "../scene.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stb_image.h>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace net::scene_stream
{
inline constexpr std::size_t max_scene_bytes=75*1024*1024;
using bytes=std::vector<std::uint8_t>;
using vec3=vk::util::math::vec3;
struct batch_payload {
    std::uint32_t token=0,batch=0;
    bool final=false;
    bytes scene;
};

namespace {
template<bool Read>
struct wire {
    static constexpr bool reading=Read;
    std::conditional_t<Read,const bytes,bytes>& buffer;
    std::size_t pos=0;
    std::size_t left() const {return buffer.size()-pos;}
    template<class T> bool field(T& value) {
        using V=std::remove_cvref_t<T>;
        if constexpr(std::is_same_v<V,std::uint32_t>) {
            if constexpr(Read) {
                if(left()<4)return false;
                value=net::movement::detail::get(buffer.data()+pos);
                pos+=4;
            } else {
                const auto old=buffer.size();
                buffer.resize(old+4);
                net::movement::detail::put(buffer.data()+old,value);
            }
            return true;
        } else if constexpr(std::is_same_v<V,float>) {
            std::uint32_t bits=0;
            if constexpr(!Read)bits=std::bit_cast<std::uint32_t>(value);
            if(!field(bits))return false;
            if constexpr(Read)value=std::bit_cast<float>(bits);
            return std::isfinite(value);
        } else if constexpr(std::is_same_v<V,bool>) {
            std::uint32_t n=0;
            if constexpr(!Read)n=value?1u:0u;
            if(!field(n)||n>1)return false;
            if constexpr(Read)value=n!=0;
            return true;
        } else if constexpr(std::is_same_v<V,vec3>) {
            return fields(value.x,value.y,value.z);
        } else if constexpr(std::is_same_v<V,std::array<float,3>> ||
                            std::is_same_v<V,std::array<float,4>> ||
                            std::is_same_v<V,std::array<float,16>> ||
                            std::is_same_v<V,std::array<std::uint32_t,4>>) {
            for(auto& x:value)if(!field(x))return false;
            return true;
        } else if constexpr(std::is_same_v<V,::scene::material>) {
            if(!fields(value.base_color,value.emissive,value.metallic,
                value.roughness,value.occlusion,value.alpha_cutoff,
                value.base_color_texture,value.metallic_roughness_texture,
                value.normal_texture,value.occlusion_texture,
                value.emissive_texture,value.alpha_mode,value.double_sided,
                value.procedural_pattern))return false;
            if constexpr(Read)
                return value.alpha_mode<=2 && value.procedural_pattern<=1;
            return true;
        } else if constexpr(std::is_same_v<V,::scene::instance>) {
            auto& pose=value.pose;
            // Both packet directions use precisely the same 108-byte schema.
            if(!fields(value.id,value.mesh_id,value.animation,value.animation_tick,
                value.overlay_animation,value.overlay_root,value.overlay_weight,
                pose.position,pose.scale,pose.rotation.x,pose.rotation.y,
                pose.rotation.z,pose.rotation.w,value.tint,value.pattern,
                value.rotation_reference,value.optical_strength,
                value.optical_mattness,value.editor_group))return false;
            if constexpr(Read) {
                return value.overlay_weight>=0 && value.overlay_weight<=1 &&
                    value.tint.x>=0 && value.tint.x<=1 &&
                    value.tint.y>=0 && value.tint.y<=1 &&
                    value.tint.z>=0 && value.tint.z<=1 &&
                    (value.pattern<=3 || value.pattern==UINT32_MAX) &&
                    value.optical_strength>=0 && value.optical_strength<=1 &&
                    value.optical_mattness>=0 && value.optical_mattness<=1;
            }
            return true;
        }
    }
    template<class... T> bool fields(T&... values) {
        return (field(values) && ...);
    }
    template<class T> bool blob(T& data,std::size_t size) {
        if constexpr(Read) {
            if(size>left())return false;
            data.assign(buffer.begin()+pos,buffer.begin()+pos+size);
            pos+=size;
        } else {
            if(data.size()!=size)return false;
            buffer.insert(buffer.end(),data.begin(),data.end());
        }
        return true;
    }
};
std::uint32_t hash(const std::uint8_t* p,std::size_t size) {
    std::uint32_t value=2166136261u;
    for(std::size_t i=0;i<size;++i)value=(value^p[i])*16777619u;
    return value;
}
bool valid_box(const vec3& a,const vec3& b) {
    return std::isfinite(a.x)&&std::isfinite(a.y)&&std::isfinite(a.z)&&
           std::isfinite(b.x)&&std::isfinite(b.y)&&std::isfinite(b.z)&&
           a.x<b.x&&a.y<b.y&&a.z<b.z;
}
bool valid_light(const ::scene::point_light& l) {
    return std::isfinite(l.radius)&&l.radius>0 &&
           std::isfinite(l.strength)&&l.strength>=0 &&
           l.color.x>=0&&l.color.y>=0&&l.color.z>=0;
}
template<bool Read,class Container,class Fn>
bool items(wire<Read>& c,Container& values,std::uint32_t count,Fn fn) {
    if constexpr(Read) {
        values.reserve(count);
        for(std::uint32_t i=0;i<count;++i) {
            typename std::remove_cvref_t<Container>::value_type value{};
            if(!fn(value))return false;
            values.push_back(std::move(value));
        }
    } else {
        for(const auto& value:values)if(!fn(value))return false;
    }
    return true;
}
template<bool Read,class Scene>
bool transcode(wire<Read>& c,Scene& scene) {
    std::uint32_t meshes=0,instances=0,textures=0,skins=0,boxes=0,lights=0;
    if constexpr(!Read) {
        meshes=std::uint32_t(scene.meshes.size());
        instances=std::uint32_t(scene.instances.size());
        textures=std::uint32_t(scene.textures.size());
        skins=std::uint32_t(scene.skins.size());
        boxes=std::uint32_t(scene.collision_boxes.size());
        lights=std::uint32_t(scene.lights.size());
        if(meshes>max_scene_bytes/12 || instances>max_scene_bytes/108 ||
           lights>64)return false;
    }
    if(!c.fields(meshes,instances,textures,skins,boxes,lights))return false;
    if constexpr(Read) {
        if(lights>64 || lights>c.left()/32 || boxes>c.left()/24 ||
           textures>c.left()/20 || skins>c.left()/16 ||
           meshes>c.left()/12 || instances>c.left()/108)return false;
    }
    if(!items(c,scene.textures,textures,[&](auto& t) {
        std::uint32_t encoded=0,length=0;
        if constexpr(!Read) {
            encoded=!t.encoded.empty();
            length=std::uint32_t(encoded?t.encoded.size():t.pixels.size());
            const auto raw=std::uint64_t(t.width)*t.height*4u;
            if(!t.id||!t.width||!t.height||raw>max_scene_bytes-20 ||
               (!encoded && raw!=t.pixels.size()) ||
               !length||length>max_scene_bytes-20 ||
               c.buffer.size()+20+length>max_scene_bytes)return false;
        }
        if(!c.fields(t.id,t.width,t.height,encoded,length))return false;
        if constexpr(Read) {
            if(!t.id||!t.width||!t.height||encoded>1 || length>c.left() ||
               std::uint64_t(t.width)*t.height*4u>max_scene_bytes-20)
                return false;
            if(!encoded) {
                if(std::uint64_t(t.width)*t.height*4u!=length)return false;
                return c.blob(t.pixels,length);
            }
            if(length>INT_MAX || t.width>INT_MAX || t.height>INT_MAX)
                return false;
            int w=0,h=0,channels=0;
            auto* decoded=stbi_load_from_memory(c.buffer.data()+c.pos,
                int(length),&w,&h,&channels,4);
            if(!decoded||w!=int(t.width)||h!=int(t.height)) {
                stbi_image_free(decoded);
                return false;
            }
            t.pixels.assign(decoded,decoded+std::size_t(w)*h*4u);
            stbi_image_free(decoded);
            c.pos+=length;
            return true;
        } else {
            return encoded?c.blob(t.encoded,length):c.blob(t.pixels,length);
        }
    }))return false;
    if(!items(c,scene.skins,skins,[&](auto& skin) {
        std::uint32_t joints=0,length=0,clips=0;
        if constexpr(!Read) {
            joints=std::uint32_t(skin.inverse_bind.size());
            length=std::uint32_t(skin.skeleton.size());
            clips=std::uint32_t(skin.clips.size());
            if(!skin.id||!length||!joints||joints>1024)return false;
        }
        if(!c.fields(skin.id,joints,length,clips))return false;
        if constexpr(Read) {
            if(!skin.id||!joints||joints>1024||length>c.left()||
               std::uint64_t(joints)*64>c.left()-length||
               clips>max_scene_bytes/8)return false;
        }
        if(!c.blob(skin.skeleton,length))return false;
        if constexpr(Read)skin.inverse_bind.resize(joints);
        for(auto& matrix:skin.inverse_bind)if(!c.field(matrix))return false;
        if(!items(c,skin.clips,clips,[&](auto& clip) {
            std::uint32_t name=0,size=0;
            if constexpr(!Read) {
                name=std::uint32_t(clip.name.size());
                size=std::uint32_t(clip.bytes.size());
                if(name>255 || !size)return false;
            }
            if(!c.field(name))return false;
            if constexpr(Read)if(name>255 || name>c.left())return false;
            if(!c.blob(clip.name,name) || !c.field(size))return false;
            if constexpr(Read)if(!size||size>c.left())return false;
            return c.blob(clip.bytes,size);
        }))return false;
        if constexpr(!Read)if(c.buffer.size()>max_scene_bytes)return false;
        return true;
    }))return false;
    if(!items(c,scene.meshes,meshes,[&](auto& mesh) {
        std::uint32_t vertices=0,indices=0;
        if constexpr(!Read) {
            vertices=std::uint32_t(mesh.vertices.size());
            indices=std::uint32_t(mesh.indices.size());
            if(!vertices||!indices||indices%3 ||
               vertices>max_scene_bytes/64 || indices>max_scene_bytes/4)
                return false;
        }
        if(!c.fields(mesh.id,mesh.skin_id,vertices,indices))return false;
        if constexpr(Read) {
            if(!vertices||!indices||indices%3||c.left()<76 ||
               std::uint64_t(vertices)*(mesh.skin_id?64u:32u)+
                   std::uint64_t(indices)*4u>c.left()-76)return false;
        }
        if(!c.field(mesh.surface))return false;
        if constexpr(Read) {
            mesh.vertices.resize(vertices);
            mesh.indices.resize(indices);
        }
        for(auto& vertex:mesh.vertices) {
            if(!c.fields(vertex.position,vertex.normal,vertex.u,vertex.v))
                return false;
            if(mesh.skin_id) {
                if(!c.fields(vertex.joints,vertex.weights))return false;
                if constexpr(Read)
                    for(auto weight:vertex.weights)
                        if(weight<0 || weight>1)return false;
            }
        }
        for(auto& index:mesh.indices) {
            if constexpr(!Read)if(index>=vertices)return false;
            if(!c.field(index))return false;
            if constexpr(Read)if(index>=vertices)return false;
        }
        if constexpr(!Read)if(c.buffer.size()>max_scene_bytes)return false;
        return true;
    }))return false;
    if(!items(c,scene.instances,instances,[&](auto& instance) {
        return c.field(instance);
    }))return false;
    if(!items(c,scene.collision_boxes,boxes,[&](auto& box) {
        return c.fields(box.minimum,box.maximum) &&
               valid_box(box.minimum,box.maximum);
    }))return false;
    if(!items(c,scene.lights,lights,[&](auto& light) {
        return c.fields(light.position,light.radius,light.color,light.strength) &&
               valid_light(light);
    }))return false;
    return true;
}
} // namespace

bytes serialize(const ::scene::state& scene) {
    bytes out{'S','C','N','B'};
    wire<false> c{out};
    if(!transcode(c,scene) || out.size()>max_scene_bytes)
        throw std::runtime_error("Invalid scene snapshot or batch too large");
    return out;
}
bool deserialize(const bytes& data,::scene::state& scene) {
    if(data.size()<28 || data.size()>max_scene_bytes ||
       data[0]!='S'||data[1]!='C'||data[2]!='N'||data[3]!='B')return false;
    wire<true> c{data,4};
    ::scene::state result;
    if(!transcode(c,result) || c.left())return false;
    scene=std::move(result);
    return true;
}
std::uint32_t checksum(const bytes& data) {
    return hash(data.data(),data.size());
}
bytes encode_batch(const bytes& data,std::uint32_t token,
                   std::uint32_t batch,bool final) {
    if(data.empty()||data.size()>max_scene_bytes||!token||batch>=65535)
        throw std::invalid_argument("invalid scene batch");
    bytes out{'V','G','S','C',1,std::uint8_t(final),0,0};
    wire<false> c{out};
    const auto sum=checksum(data);
    if(!c.fields(token,batch,sum))
        throw std::runtime_error("invalid scene batch header");
    out.insert(out.end(),data.begin(),data.end());
    return out;
}
bool decode_batch(const bytes& data,batch_payload& result) {
    if(data.size()<=20||data.size()>max_scene_bytes+20||
       data[0]!='V'||data[1]!='G'||data[2]!='S'||data[3]!='C'||
       data[4]!=1||data[5]>1||data[6]||data[7])return false;
    wire<true> c{data,8};
    std::uint32_t token=0,batch=0,expected=0;
    if(!c.fields(token,batch,expected)||!token||batch>=65535||
       expected!=hash(data.data()+20,data.size()-20))return false;
    result={token,batch,data[5]!=0,bytes(data.begin()+20,data.end())};
    return true;
}
} // namespace net::scene_stream
