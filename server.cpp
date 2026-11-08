// Two-file Vulkan game rewrite — SERVER FOUNDATION, not feature parity.
// Directly uses VulkanSystems/network; no clt/, srv/, or game headers.
// C++20. Link VulkanNetwork and its required Mbed TLS dependencies.
#include <network/secure.hpp>
#include <network/session.hpp>
#include <network/endpoint.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
using namespace std::chrono;
using bytes=std::vector<std::uint8_t>;
using clock_type=steady_clock;
namespace {
volatile std::sig_atomic_t running=1;
void stop(int){running=0;}
std::string config_dir(){
#ifdef _WIN32
    const char* p=std::getenv("APPDATA");
#else
    const char* p=std::getenv("XDG_CONFIG_HOME");
    if(!p){static std::string base=[] {const char* home=std::getenv("HOME");return std::string(home?home:".")+"/.config";}();p=base.c_str();}
#endif
    if(!p)throw std::runtime_error("No user config directory");
    return (std::filesystem::path(p)/"vulkan-game").string();
}
void u32(bytes& b,std::uint32_t x){for(int i=0;i<4;i++)b.push_back(std::uint8_t(x>>(8*i)));}
void f32(bytes& b,float x){if(!std::isfinite(x))throw std::invalid_argument("nonfinite float");std::uint32_t u;std::memcpy(&u,&x,4);u32(b,u);}
struct reader {const bytes& b;size_t i=0;bool ok=true;
    std::uint32_t u(){if(i+4>b.size()){ok=false;return 0;}std::uint32_t x=0;for(int n=0;n<4;n++)x|=std::uint32_t(b[i++])<<(8*n);return x;}
    float f(){std::uint32_t u32=u();float x;std::memcpy(&x,&u32,4);if(!std::isfinite(x))ok=false;return x;}
    bool end()const{return ok&&i==b.size();}
};
enum : std::uint32_t {hello=1,scene=2,input=3,player=4,shoot=5,reload=6,weapon=7,edit_light=8};
struct box {float x,y,z,sx,sy,sz;};
struct light {float x,y,z,r,red,green,blue,strength;};
struct actor {net::endpoint::resource peer{};float x=0,z=5,y=1.7f;std::uint32_t ack=0;
    std::uint32_t ammo=12,reserve=48;bool reloading=false;
    clock_type::time_point shot={},reload_end={},last_seen=clock_type::now();};
constexpr float radius=.35f,speed=3.8f;
std::vector<box> boxes={{0,-.1f,0,10,.1f,10},{-2,.5f,0,.5f,.5f,.5f},{0,.5f,0,.5f,.5f,.5f},{2,.5f,0,.5f,.5f,.5f}};
std::vector<light> lights={{0,3,1,6,1,.85f,.7f,1}};
std::map<std::string,actor> players;
bool collide(float x,float z){for(const auto& b:boxes){if(b.sy<.15f)continue;float nx=std::clamp(x,b.x-b.sx,b.x+b.sx),nz=std::clamp(z,b.z-b.sz,b.z+b.sz);float dx=x-nx,dz=z-nz;if(dx*dx+dz*dz<radius*radius)return true;}return false;}
void move(actor& a,int dx,int dz){float fx=float(std::clamp(dx,-1,1)),fz=float(std::clamp(dz,-1,1));float len=std::hypot(fx,fz);if(len>1){fx/=len;fz/=len;}
    float nx=a.x+fx*speed/60.f,nz=a.z+fz*speed/60.f;
    if(!collide(nx,a.z))a.x=nx;
    if(!collide(a.x,nz))a.z=nz;
    a.x=std::clamp(a.x,-9.5f,9.5f);a.z=std::clamp(a.z,-9.5f,9.5f);
}
bytes message(std::uint32_t type){bytes b;u32(b,0x324756u);u32(b,type);return b;}
bytes scene_packet(){auto b=message(scene);u32(b,std::uint32_t(boxes.size()));for(auto& v:boxes)for(float f:{v.x,v.y,v.z,v.sx,v.sy,v.sz})f32(b,f);
    u32(b,std::uint32_t(lights.size()));for(auto& l:lights)for(float f:{l.x,l.y,l.z,l.r,l.red,l.green,l.blue,l.strength})f32(b,f);return b;}
bytes player_packet(const actor& a){auto b=message(player);u32(b,a.ack);f32(b,a.x);f32(b,a.y);f32(b,a.z);return b;}
bytes weapon_packet(const actor& a){auto b=message(weapon);u32(b,a.ammo);u32(b,a.reserve);u32(b,a.reloading?1:0);return b;}
}
int main(int argc,char** argv){
    try{
        std::signal(SIGINT,stop);std::signal(SIGTERM,stop);
        const auto port=argc>1?std::uint16_t(std::stoul(argv[1])):std::uint16_t(27015);
        net::secure::settings settings{};settings.storage_directory=config_dir();settings.max_peers=64;
        auto socket=net::secure::create(net::secure::role::server,settings,port);
        net::session::settings channel{};channel.allow_unknown_peers=false;
        channel.channel.mtu=1200;net::session reliable(channel);
        std::cout<<"Server DTLS UDP :"<<port<<" @ 60 Hz\n";
        auto next=clock_type::now();std::uint64_t tick=0;std::array<std::uint8_t,1400> datagram{};
        while(running){
            // Transport controls authentication, reliability, ordering and retransmission.
            net::endpoint::resource peer{};
            while(auto n=net::secure::receive(socket,peer,datagram.data(),datagram.size())){
                if(!net::secure::authenticated(socket,peer))continue;
                reliable.open(peer);reliable.accept(peer,datagram.data(),*n);
            }
            net::delivery event{};
            while(reliable.pop(event)){
                const auto id=net::endpoint::identifier(event.peer);
                reader r{event.payload};if(r.u()!=0x324756u)continue;auto type=r.u();if(!r.ok)continue;
                if(type==hello){
                    if(!r.end())continue;
                    if(players.size()>=64&&!players.count(id))continue;
                    auto [it,created]=players.try_emplace(id);auto& p=it->second;p.peer=event.peer;
                    p.last_seen=clock_type::now();reliable.send(event.peer,scene_packet());
                    reliable.send(event.peer,player_packet(p));reliable.send(event.peer,weapon_packet(p));
                }else{
                    auto it=players.find(id);if(it==players.end())continue;auto& p=it->second;p.last_seen=clock_type::now();
                    if(type==input){auto seq=r.u();int dx=int(r.u())-1,dz=int(r.u())-1;
                        if(r.end()&&seq>p.ack&&seq-p.ack<=120){p.ack=seq;move(p,dx,dz);}
                    }else if(type==shoot){
                        if(r.end()&&!p.reloading&&p.ammo&&
                           (p.shot==clock_type::time_point{}||clock_type::now()-p.shot>=milliseconds(90))){
                            --p.ammo;p.shot=clock_type::now();reliable.send(event.peer,weapon_packet(p));
                        }
                    }else if(type==reload){
                        if(r.end()&&!p.reloading&&p.ammo<12&&p.reserve){
                            p.reloading=true;p.reload_end=clock_type::now()+milliseconds(1300);
                            reliable.send(event.peer,weapon_packet(p));
                        }
                    }else if(type==edit_light){
                        auto i=r.u();light l{};for(float* f:{&l.x,&l.y,&l.z,&l.r,&l.red,&l.green,&l.blue,&l.strength})*f=r.f();
                        if(r.end()&&i<lights.size()&&l.r>=.05f&&l.r<=64&&l.strength>=0&&l.strength<=20&&
                           std::abs(l.x)<500&&std::abs(l.y)<500&&std::abs(l.z)<500){
                            lights[i]=l;auto snapshot=scene_packet();for(auto& [_,q]:players)reliable.send(q.peer,snapshot);
                        }
                    }
                }
            }
            const auto now=clock_type::now();
            for(auto it=players.begin();it!=players.end();){auto& p=it->second;
                if(now-p.last_seen>seconds(45)){reliable.close(p.peer);it=players.erase(it);continue;}
                if(p.reloading&&now>=p.reload_end){auto n=std::min<std::uint32_t>(12-p.ammo,p.reserve);p.ammo+=n;p.reserve-=n;p.reloading=false;reliable.send(p.peer,weapon_packet(p));}
                if((tick%3)==0)reliable.send(p.peer,player_packet(p));
                ++it;
            }
            reliable.pump([&](const auto& p,const void* data,size_t length){return net::secure::send(socket,p,data,length);});
            ++tick;next+=microseconds(16667);if(next<clock_type::now()-milliseconds(100))next=clock_type::now();std::this_thread::sleep_until(next);
        }
        net::secure::destroy(socket);return 0;
    }catch(const std::exception& e){std::cerr<<"server: "<<e.what()<<'\n';return 1;}
}