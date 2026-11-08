// Two-file Vulkan game rewrite — CLIENT FOUNDATION, not feature parity.
// Direct VulkanSystems transport + PCloud world data, no clt/, srv/ headers.
// This is a headless protocol/prediction proof of concept, NOT a renderer.
// C++20. Link VulkanNetwork (Mbed TLS) and VulkanPCloud + its modules.
#include <network/secure.hpp>
#include <network/session.hpp>
#include <network/endpoint.hpp>
#include <pcloud/pcloud.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <sstream>
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
    float f(){std::uint32_t bits=u();float value;std::memcpy(&value,&bits,4);if(!std::isfinite(value))ok=false;return value;}
    bool end()const{return ok&&i==b.size();}
};
enum : std::uint32_t {hello=1,scene=2,input=3,player=4,shoot=5,reload=6,weapon=7,edit_light=8};
struct box {float x,y,z,sx,sy,sz;};
struct light {float x,y,z,r,red,green,blue,strength;};
struct move_record {std::uint32_t seq;int dx,dz;};
struct state {
    std::vector<box> boxes;std::vector<light> lights;
    std::deque<move_record> history;std::uint32_t seq=0,ack=0;
    float x=0,y=1.7f,z=5,render_x=0,render_z=5;
    std::uint32_t ammo=12,reserve=48;bool reloading=false,got_scene=false;
    int dx=0,dz=0;bool trigger=false,request_reload=false;
    clock_type::time_point last_hello{},last_fire{},last_print{};
};
std::mutex commands_lock;std::deque<std::string> commands;
bytes message(std::uint32_t type){bytes b;u32(b,0x324756u);u32(b,type);return b;}
bool collide(const state& s,float x,float z){for(const auto& b:s.boxes){if(b.sy<.15f)continue;float nx=std::clamp(x,b.x-b.sx,b.x+b.sx),nz=std::clamp(z,b.z-b.sz,b.z+b.sz);float dx=x-nx,dz=z-nz;if(dx*dx+dz*dz<.35f*.35f)return true;}return false;}
void move(state& s,int dx,int dz){float fx=float(std::clamp(dx,-1,1)),fz=float(std::clamp(dz,-1,1));float n=std::hypot(fx,fz);if(n>1){fx/=n;fz/=n;}
    float x=s.x+fx*3.8f/60.f,z=s.z+fz*3.8f/60.f;
    if(!collide(s,x,s.z))s.x=x;
    if(!collide(s,s.x,z))s.z=z;
    s.x=std::clamp(s.x,-9.5f,9.5f);s.z=std::clamp(s.z,-9.5f,9.5f);
}
// Converts the game's plain arrays into PCloud's ABI directly. Rendering
// still requires Vulkan device + swapchain + matching shader-readable Gbuffer.
std::shared_ptr<pc::world> lighting_scene(const state& s){
    auto world=std::make_shared<pc::world>();
    for(auto& l:s.lights)world->lights.push_back({{l.x,l.y,l.z},l.r,{l.red,l.green,l.blue},l.strength});
    for(auto& b:s.boxes){world->collision_boxes.push_back({{b.x-b.sx,b.y-b.sy,b.z-b.sz},{b.x+b.sx,b.y+b.sy,b.z+b.sz}});
        float x=b.x,y=b.y,z=b.z,a=b.sx,c=b.sy,d=b.sz;
        const std::array<pc::vec3,8> v={pc::vec3{x-a,y-c,z-d},{x+a,y-c,z-d},{x+a,y+c,z-d},{x-a,y+c,z-d},
            {x-a,y-c,z+d},{x+a,y-c,z+d},{x+a,y+c,z+d},{x-a,y+c,z+d}};
        const int faces[6][4]={{0,3,2,1},{4,5,6,7},{0,4,7,3},{1,2,6,5},{0,1,5,4},{3,7,6,2}};
        for(auto& f:faces){world->triangles.push_back({v[f[0]],v[f[1]],v[f[2]]});world->triangles.push_back({v[f[0]],v[f[2]],v[f[3]]});}
    }
    return world;
}
void interpret(state& s,const std::string& line,net::session& reliable,const net::endpoint::resource& peer){
    std::istringstream in(line);std::string key;in>>key;
    if(key=="w")s.dz=-1;else if(key=="s")s.dz=1;else if(key=="a")s.dx=-1;
    else if(key=="d")s.dx=1;else if(key=="stop")s.dx=s.dz=0;
    else if(key=="f")s.trigger=true;else if(key=="r")s.request_reload=true;
    else if(key=="exit"||key=="quit")running=0;
    else if(key=="light"){
        unsigned index;light l{};
        if(in>>index>>l.x>>l.y>>l.z>>l.r>>l.red>>l.green>>l.blue>>l.strength){
            bytes p=message(edit_light);u32(p,index);
            for(float v:{l.x,l.y,l.z,l.r,l.red,l.green,l.blue,l.strength})f32(p,v);
            reliable.send(peer,std::move(p));
        }
    }
}
}
int main(int argc,char** argv){
    try{
        std::signal(SIGINT,stop);std::signal(SIGTERM,stop);
        const std::string host=argc>1?argv[1]:"127.0.0.1";
        const auto port=argc>2?std::uint16_t(std::stoul(argv[2])):std::uint16_t(27015);
        net::secure::settings cfg{};cfg.storage_directory=config_dir();cfg.trust_identity=host+":"+std::to_string(port);cfg.max_peers=1;
        auto socket=net::secure::create(net::secure::role::client,cfg);
        const auto peer=net::endpoint::resolve(host,port);net::secure::connect(socket,peer);
        net::session::settings options{};options.allow_unknown_peers=false;
        options.channel.mtu=1200;net::session reliable(options);reliable.open(peer);
        state s{};s.last_print=clock_type::now();
        std::thread([]{std::string line;while(std::getline(std::cin,line)){std::lock_guard<std::mutex> lock(commands_lock);commands.push_back(std::move(line));}}).detach();
        std::cout<<"Connecting via DTLS: "<<host<<':'<<port<<"\n"
                    "Commands: w/a/s/d (persistent direction), stop, f, r, light INDEX X Y Z R RED GREEN BLUE STRENGTH, exit\n";
        auto next=clock_type::now();std::array<std::uint8_t,1400> packet{};
        while(running){
            net::endpoint::resource incoming{};
            while(auto n=net::secure::receive(socket,incoming,packet.data(),packet.size())){
                if(!net::endpoint::equal(peer,incoming) || !net::secure::authenticated(socket,peer))continue;
                reliable.accept(peer,packet.data(),*n);
            }
            if(net::secure::authenticated(socket,peer) &&
               (!s.got_scene)&&(s.last_hello==clock_type::time_point{}||clock_type::now()-s.last_hello>seconds(2))){
                reliable.send(peer,message(hello));s.last_hello=clock_type::now();
            }
            net::delivery event{};
            while(reliable.pop(event)){
                if(!net::endpoint::equal(event.peer,peer))continue;
                reader r{event.payload};if(r.u()!=0x324756u)continue;auto type=r.u();if(!r.ok)continue;
                if(type==scene){
                    auto count=r.u();if(count>4096)continue;std::vector<box> b(count);
                    for(auto& item:b)for(float* v:{&item.x,&item.y,&item.z,&item.sx,&item.sy,&item.sz})*v=r.f();
                    auto count_lights=r.u();if(count_lights>64)continue;std::vector<light> l(count_lights);
                    for(auto& item:l)for(float* v:{&item.x,&item.y,&item.z,&item.r,&item.red,&item.green,&item.blue,&item.strength})*v=r.f();
                    if(!r.end())continue;
                    s.boxes=std::move(b);s.lights=std::move(l);s.got_scene=true;
                    auto world=lighting_scene(s);
                    std::cout<<"Scene: "<<s.boxes.size()<<" boxes, "<<s.lights.size()<<" lights, "<<world->triangles.size()<<" PCloud triangles (GPU not initialized)\n";
                }else if(type==player){
                    auto ack=r.u();float x=r.f(),y=r.f(),z=r.f();if(!r.end()||ack<s.ack)continue;
                    s.ack=ack;s.x=x;s.y=y;s.z=z;
                    while(!s.history.empty()&&s.history.front().seq<=ack)s.history.pop_front();
                    for(auto& record:s.history)move(s,record.dx,record.dz);
                }else if(type==weapon){
                    auto ammo=r.u(),reserve=r.u(),reloading=r.u();if(!r.end())continue;
                    s.ammo=ammo;s.reserve=reserve;s.reloading=reloading!=0;
                }
            }
            std::deque<std::string> queued;{std::lock_guard<std::mutex> lock(commands_lock);queued.swap(commands);}
            for(auto& cmd:queued)interpret(s,cmd,reliable,peer);
            if(s.got_scene&&net::secure::authenticated(socket,peer)){
                bytes p=message(input);u32(p,++s.seq);u32(p,std::uint32_t(s.dx+1));u32(p,std::uint32_t(s.dz+1));
                if(reliable.send(peer,std::move(p))){s.history.push_back({s.seq,s.dx,s.dz});move(s,s.dx,s.dz);}
                else --s.seq;
                if(s.history.size()>120)s.history.pop_front();
                if(s.trigger){reliable.send(peer,message(shoot));s.trigger=false;}
                if(s.request_reload){reliable.send(peer,message(reload));s.request_reload=false;}
                s.render_x+=.2f*(s.x-s.render_x);s.render_z+=.2f*(s.z-s.render_z);
            }
            reliable.pump([&](const auto& to,const void* data,size_t len){return net::secure::send(socket,to,data,len);});
            if(clock_type::now()-s.last_print>seconds(2)){
                std::cout<<"pos=("<<s.render_x<<","<<s.y<<","<<s.render_z<<")"
                    <<" ack="<<s.ack<<" ammo="<<s.ammo<<"/"<<s.reserve
                    <<(s.reloading?" reloading":"")<<"\n";s.last_print=clock_type::now();
            }
            next+=microseconds(16667);if(next<clock_type::now()-milliseconds(100))next=clock_type::now();std::this_thread::sleep_until(next);
        }
        net::secure::destroy(socket);return 0;
    }catch(const std::exception& e){std::cerr<<"client: "<<e.what()<<'\n';return 1;}
}