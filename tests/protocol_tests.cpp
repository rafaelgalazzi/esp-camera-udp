#include "reassembly.hpp"
#include <iostream>
#include <stdexcept>
#include <array>
#define CHECK(x)                                     \
    do                                               \
    {                                                \
        if (!(x))                                    \
            throw std::runtime_error("Failed: " #x); \
    } while (0)
using namespace iris;
static Header header(uint32_t frame = 1)
{
    Header h;
    h.type = Type::jpeg;
    h.camera = 0x123456789abcULL;
    h.boot = 42;
    h.server = 23;
    h.frame = frame;
    h.total = 2404;
    h.count = 3;
    h.payload = 1200;
    h.capture_us = 123456;
    return h;
}
static bool feed(Reassembler &r, Header h, unsigned index, uint64_t time)
{
    std::array<uint8_t, 1200> p{};
    h.index = uint16_t(index);
    h.payload = index == 2 ? 4 : 1200;
    if (index == 0)
    {
        p[0] = 0xff;
        p[1] = 0xd8;
    }
    if (index == 2)
    {
        p[2] = 0xff;
        p[3] = 0xd9;
    }
    return r.push(h, p.data(), time, 100);
}
int main()
{
    try
    {
        auto h = header();
        std::array<uint8_t, 1248> wire{};
        encode(wire.data(), h);
        Header decoded;
        CHECK(decode(wire.data(), wire.size(), decoded));
        CHECK(decoded.camera == h.camera);
        CHECK(decoded.capture_us == h.capture_us);
        CHECK(wire[8] == 0 && wire[10] == 0x12 && wire[15] == 0xbc);
        CHECK(!decode(wire.data(), 47, decoded));
        CHECK(!decode(wire.data(), wire.size() - 1, decoded));
        wire[4] = 2;
        CHECK(!decode(wire.data(), wire.size(), decoded));
        wire[4] = 1;
        auto bad = h;
        bad.total = 0xffffffff;
        encode(wire.data(), bad);
        CHECK(!decode(wire.data(), wire.size(), decoded));
        bad = h;
        bad.index = 3;
        encode(wire.data(), bad);
        CHECK(!decode(wire.data(), wire.size(), decoded));
        Reassembler r;
        CHECK(!feed(r, h, 2, 1000));
        CHECK(!feed(r, h, 0, 1001));
        CHECK(!feed(r, h, 0, 1002));
        CHECK(feed(r, h, 1, 1003));
        CHECK(r.stats.duplicates == 1 && r.stats.completed == 1 && r.latest->size() == 2404);
        auto saved = r.latest;
        CHECK(!feed(r, h, 0, 1004));
        CHECK(r.stats.stale == 1);
        CHECK(!feed(r, header(2), 0, 1010));
        r.expire(1110, 100);
        CHECK(r.stats.dropped == 1 && r.stats.missing == 2);
        CHECK(!feed(r, header(2), 1, 1111));
        CHECK(r.latest == saved);
        CHECK(!feed(r, header(3), 0, 1120));
        CHECK(!feed(r, header(4), 1, 1121));
        CHECK(!feed(r, header(3), 2, 1122));
        CHECK(!feed(r, header(4), 0, 1123));
        CHECK(feed(r, header(4), 2, 1124));
        CHECK(r.stats.dropped == 2 && r.stats.completed == 2);
        r.reset();
        CHECK(!r.latest);
        CHECK(!feed(r, header(0xfffffffe), 0, 1200));
        CHECK(!feed(r, header(0xffffffff), 0, 1201));
        CHECK(!feed(r, header(0), 0, 1202));
        CHECK(!feed(r, header(0), 1, 1203));
        CHECK(feed(r, header(0), 2, 1204));
        CHECK(newer(0, 0xffffffff));
        CHECK(!newer(0xffffffff, 0));
        CHECK(!newer(0x80000000, 0));
        // Metadata mismatch must not corrupt an in-flight frame.
        Reassembler m;
        CHECK(!feed(m, h, 0, 100));
        bad = h;
        bad.capture_us++;
        CHECK(!feed(m, bad, 1, 101));
        CHECK(m.stats.invalid == 1);
        CHECK(!feed(m, h, 1, 102));
        CHECK(feed(m, h, 2, 103));
        // Invalid JPEG is never published.
        Reassembler invalid;
        std::array<uint8_t, 4> junk{};
        bad = h;
        bad.total = 4;
        bad.count = 1;
        bad.payload = 4;
        CHECK(!invalid.push(bad, junk.data(), 1, 100));
        CHECK(!invalid.latest && invalid.stats.invalid == 1);
        std::cout << "Protocol/reassembly tests passed\n";
    }
    catch (const std::exception &e)
    {
        std::cerr << e.what() << "\n";
        return 1;
    }
}
