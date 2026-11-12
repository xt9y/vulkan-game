#pragma once
#include <bit>
#include <cstring>
#include <cstddef>
#include <cstdint>

namespace net::movement::detail {
inline void put(std::uint8_t* p,std::uint32_t n) {
            p[0]=std::uint8_t(n>>24);p[1]=std::uint8_t(n>>16);
            p[2]=std::uint8_t(n>>8);p[3]=std::uint8_t(n);
        }
        inline std::uint32_t get(const std::uint8_t* p) {
return (std::uint32_t(p[0])<<24)|(std::uint32_t(p[1])<<16)| (std::uint32_t(p[2])<<8)|std::uint32_t(p[3]);
        }
        inline void put_float(std::uint8_t* p,float v) {
            put(p,std::bit_cast<std::uint32_t>(v));
        }
        inline float get_float(const std::uint8_t* p) {
            return std::bit_cast<float>(get(p));
        }
        inline void write(std::uint8_t* p,std::uint32_t v){put(p,v);}
        inline void write(std::uint8_t* p,float v){put_float(p,v);}
        inline void read(const std::uint8_t* p,std::uint32_t& v){v=get(p);}
        inline void read(const std::uint8_t* p,float& v){v=get_float(p);}
        template<class... T>
        void write_fields(std::uint8_t* p,const T&... values) {
            ((write(p,values),p+=4),...);
        }
        template<class... T>
        void read_fields(const std::uint8_t* p,T&... values) {
            ((read(p,values),p+=4),...);
        }
        inline bool header(const std::uint8_t* p,std::size_t n,std::uint8_t kind) {
return p&&n>=8&&p[0]=='V'&&p[1]=='M'&&p[2]=='O'&&p[3]=='V'&& p[4]==1&&p[5]==kind&&p[6]==0&&p[7]==0;
        }

} // namespace net::movement::detail
