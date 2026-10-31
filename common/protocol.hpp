#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <algorithm>

namespace iris {
constexpr size_t header_size = 48, chunk_size = 1200, max_frame_size = 256 * 1024;
constexpr uint16_t control_port = 5001;
enum class Type : uint8_t { hello = 1, accept = 2, jpeg = 3 };
inline uint64_t read_be(const uint8_t* p, size_t n) {
    uint64_t v = 0; while (n--) v = (v << 8) | *p++; return v;
}
inline void write_be(uint8_t* p, uint64_t v, size_t n) {
    while (n) { p[--n] = uint8_t(v); v >>= 8; }
}
struct Header {
    Type type = Type::hello;
    uint16_t payload = 0;
    uint64_t camera = 0;
    uint32_t boot = 0, frame = 0;
    uint64_t capture_us = 0;
    uint16_t index = 0, count = 0;
    uint32_t total = 0;
    uint64_t server = 0;
};
inline void encode(uint8_t* p, const Header& h) {
    std::memcpy(p, "OIRB", 4); p[4] = 1; p[5] = uint8_t(h.type);
    write_be(p+6,h.payload,2); write_be(p+8,h.camera,8); write_be(p+16,h.boot,4);
    write_be(p+20,h.frame,4); write_be(p+24,h.capture_us,8);
    write_be(p+32,h.index,2); write_be(p+34,h.count,2);
    write_be(p+36,h.total,4); write_be(p+40,h.server,8);
}
inline bool decode(const uint8_t* p, size_t n, Header& h) {
    if(n < header_size || std::memcmp(p,"OIRB",4) || p[4]!=1 || p[5]<1 || p[5]>3) return false;
    h.type=Type(p[5]); h.payload=uint16_t(read_be(p+6,2));
    if(n != header_size+h.payload || h.payload>chunk_size) return false;
    h.camera=read_be(p+8,8); h.boot=uint32_t(read_be(p+16,4)); h.frame=uint32_t(read_be(p+20,4));
    h.capture_us=read_be(p+24,8); h.index=uint16_t(read_be(p+32,2)); h.count=uint16_t(read_be(p+34,2));
    h.total=uint32_t(read_be(p+36,4)); h.server=read_be(p+40,8);
    if(!h.camera || !h.boot) return false;
    if(h.type==Type::jpeg) {
        if(h.total<4 || h.total>max_frame_size || h.count!=(h.total+chunk_size-1)/chunk_size || h.index>=h.count) return false;
        if(h.payload!=std::min(chunk_size,size_t(h.total)-h.index*chunk_size)) return false;
    } else if(h.type==Type::accept && h.payload!=4) return false;
    else if(h.type==Type::hello && h.payload>128) return false;
    return true;
}
inline bool newer(uint32_t a, uint32_t b) { return a!=b && uint32_t(a-b)<0x80000000u; }
}
