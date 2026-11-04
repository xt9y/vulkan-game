#include "blood_events.hpp"

#include <bld/bld.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <unordered_map>
#include <unordered_set>

namespace clt::blood_events
{
    namespace
    {
        using namespace vk::util::math;

        struct cache
        {
            std::shared_ptr<const ::scene::state> source;
            std::unordered_map<std::uint32_t,const ::scene::mesh*> meshes;
            std::unordered_map<std::uint32_t,const ::scene::instance*> instances;
            std::unordered_set<std::uint32_t> registered;
        };

        cache& data()
        {
            static cache value;
            return value;
        }

        void index(const std::shared_ptr<const ::scene::state>& source)
        {
            auto& d=data();
            if(d.source==source)return;
            d.source=source;
            d.meshes.clear();
            d.instances.clear();
            if(!source)return;
            d.meshes.reserve(source->meshes.size());
            d.instances.reserve(source->instances.size());
            for(const auto& m:source->meshes)d.meshes.emplace(m.id,&m);
            for(const auto& i:source->instances)d.instances.emplace(i.id,&i);
        }

        struct receiver
        {
            const ::scene::instance* instance=nullptr;
            const ::scene::mesh* mesh=nullptr;
        };

        receiver find(std::uint32_t id)
        {
            const auto& d=data();
            const auto i=d.instances.find(id);
            if(i==d.instances.end())return {};
            const auto m=d.meshes.find(i->second->mesh_id);
            return {i->second,m==d.meshes.end()?nullptr:m->second};
        }

        struct coords
        {
            bld::vec2 uv{};
            bld::vec2 metric{1.f,1.f};
            bool valid=false;
        };

        coords coordinates(const receiver& r,const ::weapon::hit& hit)
        {
            coords result{};
            if(!r.instance || !r.mesh)return result;
            const auto& mesh=*r.mesh;
            const auto t=std::size_t(hit.triangle)*3;
            if(t+2>=mesh.indices.size())return result;
            const auto i0=mesh.indices[t],i1=mesh.indices[t+1],i2=mesh.indices[t+2];
            if(i0>=mesh.vertices.size() ||
               i1>=mesh.vertices.size() ||
               i2>=mesh.vertices.size())return result;
            const auto& a=mesh.vertices[i0];
            const auto& b=mesh.vertices[i1];
            const auto& c=mesh.vertices[i2];
            const auto& pose=r.instance->pose;
            const auto world=trs(pose.position,pose.rotation,pose.scale);
            const auto position=[&](vec3 local)
            {
                const vec4 p=world*vec4{local.x,local.y,local.z,1.f};
                return vec3{p.x,p.y,p.z};
            };
            const auto A=position(a.position),B=position(b.position),C=position(c.position);
            const vec3 e0=B-A,e1=C-A,p=hit.position-A;
            // Use relative triangle area and double precision. An absolute
            // 1e-12 cutoff incorrectly rejected small character triangles.
            const auto dot64=[](vec3 x,vec3 y)->double
            {
                return double(x.x)*y.x+double(x.y)*y.y+double(x.z)*y.z;
            };
            const double d00=dot64(e0,e0),d01=dot64(e0,e1);
            const double d11=dot64(e1,e1);
            const double d20=dot64(p,e0),d21=dot64(p,e1);
            const double denom=d00*d11-d01*d01;
            if(!std::isfinite(denom) || denom<=0. ||
               denom<=d00*d11*1e-12)return result;
            const double v=(d11*d20-d01*d21)/denom;
            const double w=(d00*d21-d01*d20)/denom;
            const double u=1.-v-w;
            if(!std::isfinite(u) || !std::isfinite(v) || !std::isfinite(w) ||
               u<-.12 || v<-.12 || w<-.12)return result;
            const float cu=std::clamp(float(u),0.f,1.f);
            const float cv=std::clamp(float(v),0.f,1.f);
            const float cw=std::clamp(float(w),0.f,1.f);
            const float sum=cu+cv+cw;
            if(sum<.0001f)return result;
            const float x=(a.u*cu+b.u*cv+c.u*cw)/sum;
            const float y=(a.v*cu+b.v*cv+c.v*cw)/sum;
            if(!std::isfinite(x) || !std::isfinite(y))return result;
            // glTF materials can repeat UVs beyond [0,1]. The shader now
            // wraps the same coordinates instead of clamping atlas sampling.
            result.uv={x-std::floor(x),y-std::floor(y)};
            const float du1=b.u-a.u,du2=c.u-a.u;
            const float dv1=b.v-a.v,dv2=c.v-a.v;
            const float det=du1*dv2-du2*dv1;
            if(std::abs(det)>1e-9f)
            {
                result.metric.x=std::clamp(
                    length((e0*dv2-e1*dv1)/det),.001f,100.f);
                result.metric.y=std::clamp(
                    length((e1*du1-e0*du2)/det),.001f,100.f);
            }
            result.valid=true;
            return result;
        }

        coords attach(const receiver& r,const ::weapon::hit& hit)
        {
            const auto uv=coordinates(r,hit);
            if(!r.instance || !r.mesh || !bld::surface::get())return uv;
            const auto& pose=r.instance->pose;
            bld::surface::register_target(
                r.instance->id,
                r.mesh->bleeds ? bld::substrate::textile : bld::substrate::painted,
                trs(pose.position,pose.rotation,pose.scale),
                r.mesh->skin_id!=0,uv.metric);
            data().registered.insert(r.instance->id);
            // Register the real UV-to-surface chart once. This lets the
            // film solver use local mesh normals and tangent directions to
            // follow curved hands/arms instead of sliding a 2D stamp down UV.
            const auto *atlas=bld::surface::find(r.instance->id);
            if(r.mesh->bleeds && uv.valid &&
               (!atlas || atlas->film.geometry.empty()))
            {
                std::vector<fld::surface::chart_vertex> chart;
                chart.reserve(r.mesh->vertices.size());
                for(const auto &vertex:r.mesh->vertices)
                    chart.push_back({vertex.position,vertex.normal,
                                     {vertex.u,vertex.v}});
                bld::surface::register_geometry(
                    r.instance->id,chart,r.mesh->indices);
            }
            return uv;
        }
    }

    void shot(const ::weapon::hit& hit,
              const std::shared_ptr<const ::scene::state>& source)
    {
        if(!bld::system::get())return;
        if(!hit.impacted || !source)
        {
#ifndef NDEBUG
            std::clog << "blood: shot " << (hit.impacted ? "has no scene" : "missed") << '\n';
#endif
            return;
        }
        index(source);
        const auto target=find(hit.instance);
        // Blood is an explicit game-owned property, independent of skinning.
        // Static/rigid character meshes must bleed just like animated ones.
        if(!target.mesh || !target.mesh->bleeds)
        {
#ifndef NDEBUG
            std::clog << "blood: hit instance " << hit.instance
                      << " (not a bleedable mesh)\n";
#endif
            return;
        }
        const auto uv=attach(target,hit);
        bld::impact event{};
        event.seed=0;
        event.target=hit.instance;
        event.position=hit.position;
        event.normal=hit.normal;
        // A bullet wound ejects liquid outward along the surface normal;
        // it does not inherit arbitrary world +Z as its direction.
        event.direction=hit.normal;
        event.amount_ml=6.f;
        event.velocity=3.0f;
        event.uv=uv.uv;
        event.uv_valid=uv.valid;
        bld::system::hit(event);
#ifndef NDEBUG
        const auto *solver=fld::system::get();
        const auto *atlas=bld::surface::find(hit.instance);
        std::clog << "blood: hit instance " << hit.instance
                  << " uv=" << uv.valid
                  << " particles=" << (solver ? solver->particles.size() : 0)
                  << " stains=" << (atlas ? atlas->marks.size() : 0)
                  << '\n';
#endif
    }

    void update(float seconds,const ::weapon::world& collision,
                const std::shared_ptr<const ::scene::state>& source)
    {
        if(!bld::system::get())return;
        bld::system::update(seconds,
            [&](bld::vec3 from,bld::vec3 to,float radius)->fld::collision
            {
                const auto delta=to-from;
                const auto distance=length(delta);
                if(distance<.00001f)return {};
                const auto hit=collision.trace(from,delta/distance,
                                               distance+radius);
                if(!hit.impacted)return {};
                index(source);
                const auto target=find(hit.instance);
                const auto uv=attach(target,hit);
                fld::collision contact{};
                contact.hit=true;
                contact.target=hit.instance;
                contact.position=hit.position;
                contact.normal=hit.normal;
                contact.uv=uv.uv;
                contact.uv_valid=uv.valid;
                return contact;
            });
    }

    void reset()
    {
        if(bld::system::get())bld::system::clear();
        for(const auto id:data().registered)bld::surface::remove_target(id);
        data()={};
    }
}
