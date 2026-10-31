#pragma once
#include "protocol.hpp"
#include <vector>
#include <memory>

namespace iris
{
    struct Counters
    {
        uint64_t packets = 0, bytes = 0, started = 0, completed = 0, dropped = 0, missing = 0, duplicates = 0, stale = 0, invalid = 0;
    };
    // One in-flight frame and one immutable published frame per camera.
    class Reassembler
    {
        bool seen_ = false, active_ = false;
        Header current_{};
        uint64_t since_ = 0;
        size_t received_ = 0;
        std::vector<uint8_t> buffer_, present_;
        void discard()
        {
            if (active_)
            {
                ++stats.dropped;
                stats.missing += current_.count - received_;
            }
            active_ = false;
            buffer_.clear();
            present_.clear();
        }

    public:
        Counters stats;
        uint64_t last_complete = 0, generation = 0;
        std::shared_ptr<const std::vector<uint8_t>> latest;
        void reset()
        {
            discard();
            seen_ = false;
            latest.reset();
            last_complete = 0;
        }
        void expire(uint64_t now, uint64_t timeout)
        {
            if (active_ && now - since_ >= timeout)
                discard();
        }
        bool push(const Header &h, const uint8_t *payload, uint64_t now, uint64_t timeout)
        {
            expire(now, timeout);
            ++stats.packets;
            stats.bytes += header_size + h.payload;
            if (seen_ && h.frame != current_.frame && !newer(h.frame, current_.frame))
            {
                ++stats.stale;
                return false;
            }
            if (!seen_ || h.frame != current_.frame)
            {
                discard();
                current_ = h;
                seen_ = active_ = true;
                since_ = now;
                received_ = 0;
                buffer_.resize(h.total);
                present_.assign(h.count, 0);
                ++stats.started;
            }
            if (!active_)
            {
                ++stats.stale;
                return false;
            }
            if (h.total != current_.total || h.count != current_.count || h.capture_us != current_.capture_us)
            {
                ++stats.invalid;
                return false;
            }
            if (present_[h.index])
            {
                ++stats.duplicates;
                return false;
            }
            std::memcpy(buffer_.data() + h.index * chunk_size, payload, h.payload);
            present_[h.index] = 1;
            ++received_;
            if (received_ != h.count)
                return false;
            if (buffer_[0] != 0xff || buffer_[1] != 0xd8 || buffer_[buffer_.size() - 2] != 0xff || buffer_.back() != 0xd9)
            {
                ++stats.invalid;
                discard();
                return false;
            }
            latest = std::make_shared<const std::vector<uint8_t>>(std::move(buffer_));
            active_ = false;
            ++stats.completed;
            ++generation;
            last_complete = now;
            return true;
        }
    };
}
