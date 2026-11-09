#pragma once
#include <cstddef>
#include <cstdint>

namespace net::movement::detail {
    inline void put(std::uint8_t* p,std::uint32_t value);
    inline std::uint32_t get(const std::uint8_t* p);
    inline void put_float(std::uint8_t* p,float value);
    inline float get_float(const std::uint8_t* p);
    inline void write(std::uint8_t* p,std::uint32_t value);
    inline void write(std::uint8_t* p,float value);
    inline void read(const std::uint8_t* p,std::uint32_t& value);
    inline void read(const std::uint8_t* p,float& value);
    template<class... T> void write_fields(std::uint8_t* p,const T&... values);
    template<class... T> void read_fields(const std::uint8_t* p,T&... values);
    inline bool header(const std::uint8_t* p,std::size_t size,std::uint8_t kind);
} // namespace net::movement::detail
