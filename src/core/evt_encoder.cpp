#include "core/evt_encoder.h"

#include <algorithm>
#include <cstring>

namespace evs {

const char *to_string(EvtFormat format) {
    switch (format) {
    case EvtFormat::Evt2:
        return "EVT2";
    case EvtFormat::Evt21:
        return "EVT21";
    case EvtFormat::Evt3:
        return "EVT3";
    }
    return "?";
}

const char *display_name(EvtFormat format) {
    switch (format) {
    case EvtFormat::Evt2:
        return "EVT 2.0";
    case EvtFormat::Evt21:
        return "EVT 2.1";
    case EvtFormat::Evt3:
        return "EVT 3.0";
    }
    return "?";
}

std::string evt_version(EvtFormat format) {
    switch (format) {
    case EvtFormat::Evt2:
        return "2.0";
    case EvtFormat::Evt21:
        return "2.1";
    case EvtFormat::Evt3:
        return "3.0";
    }
    return "?";
}

std::string format_descriptor(EvtFormat format, int width, int height) {
    std::string s = std::string(to_string(format)) + ";height=" + std::to_string(height) +
                    ";width=" + std::to_string(width);
    if (format == EvtFormat::Evt21)
        s += ";endianness=legacy";
    return s;
}

namespace {

template<typename T>
inline void push(std::vector<uint8_t> &out, T word) {
    const size_t pos = out.size();
    out.resize(pos + sizeof(T));
    std::memcpy(out.data() + pos, &word, sizeof(T)); // x86/x64: little endian, as the RAW formats
}

// Collects the events of one timestamp, then hands the group to the concrete encoder.
class GroupingEncoder : public EventEncoder {
public:
    void encode(const EventCD *begin, const EventCD *end, std::vector<uint8_t> &out) override {
        for (auto it = begin; it != end; ++it) {
            EventCD ev = *it;
            if (has_last_ && ev.t < last_t_)
                ev.t = last_t_;
            if (!group_.empty() && ev.t != group_.front().t)
                emit_group(out);
            group_.push_back(ev);
            last_t_   = ev.t;
            has_last_ = true;
        }
    }
    void encode_trigger(const EventExtTrigger &trigger, std::vector<uint8_t> &out) override {
        timestamp t = trigger.t;
        if (has_last_ && t < last_t_)
            t = last_t_;
        if (!group_.empty())
            emit_group(out);
        encode_trigger_at(t, trigger.p ? 1 : 0, trigger.id, out);
        last_t_   = t;
        has_last_ = true;
    }
    void flush(std::vector<uint8_t> &out) override {
        if (!group_.empty())
            emit_group(out);
    }

protected:
    virtual void encode_group(std::vector<EventCD> &group, std::vector<uint8_t> &out) = 0;
    virtual void encode_trigger_at(timestamp t, int polarity, int id, std::vector<uint8_t> &out) = 0;

private:
    void emit_group(std::vector<uint8_t> &out) {
        encode_group(group_, out);
        group_.clear();
    }
    std::vector<EventCD> group_;
    timestamp last_t_ = 0;
    bool has_last_    = false;
};

// EVT 2.x time high: 28 bits of t >> 6 (wraps every 2^34 us). Decoders detect a loop when the time high goes from
// the maximum value back to (near) zero, so each loop crossing is emitted as TH(max), TH(0).
template<typename EmitTimeHigh>
void evt2x_time_high(timestamp t, bool &started, timestamp &last_th, EmitTimeHigh emit) {
    constexpr timestamp kMask = (timestamp(1) << 28) - 1;
    const timestamp th        = t >> 6;
    if (!started) {
        emit(static_cast<uint32_t>(th & kMask));
        last_th = th;
        started = true;
        return;
    }
    if (th == last_th)
        return;
    for (timestamp loop = last_th >> 28; loop < (th >> 28); ++loop) {
        emit(static_cast<uint32_t>(kMask));
        emit(0u);
    }
    if ((th & kMask) != 0 || (th >> 28) == (last_th >> 28))
        emit(static_cast<uint32_t>(th & kMask));
    last_th = th;
}

// ------------------------------------------------------------------------------------------------ EVT 2.0
class Evt2Encoder final : public GroupingEncoder {
public:
    EvtFormat format() const override { return EvtFormat::Evt2; }

protected:
    void encode_group(std::vector<EventCD> &group, std::vector<uint8_t> &out) override {
        evt2x_time_high(group.front().t, started_, last_th_,
                        [&](uint32_t th) { push<uint32_t>(out, (0x8u << 28) | th); });
        for (const auto &ev : group) {
            const uint32_t w = (static_cast<uint32_t>(ev.p ? 0x1 : 0x0) << 28) |
                               (static_cast<uint32_t>(ev.t & 0x3F) << 22) | ((ev.x & 0x7FFu) << 11) | (ev.y & 0x7FFu);
            push<uint32_t>(out, w);
        }
    }
    // EXT_TRIGGER: [type 0xA:4][ts:6][unused:9][id:5][unused:7][value:1]
    void encode_trigger_at(timestamp t, int polarity, int id, std::vector<uint8_t> &out) override {
        evt2x_time_high(t, started_, last_th_, [&](uint32_t th) { push<uint32_t>(out, (0x8u << 28) | th); });
        push<uint32_t>(out, (0xAu << 28) | (static_cast<uint32_t>(t & 0x3F) << 22) | ((id & 0x1Fu) << 8) |
                                (polarity & 1u));
    }

private:
    bool started_      = false;
    timestamp last_th_ = 0;
};

// ------------------------------------------------------------------------------------------------ EVT 2.1 (legacy)
// 64-bit words; "legacy" layout: bits 0..31 = [type:4][ts:6][x:11][y:11], bits 32..63 = valid mask.
// A CD word reports up to 32 pixels x_base + i (x_base multiple of 32) of one row, one polarity.
class Evt21Encoder final : public GroupingEncoder {
public:
    EvtFormat format() const override { return EvtFormat::Evt21; }

protected:
    void encode_group(std::vector<EventCD> &group, std::vector<uint8_t> &out) override {
        evt2x_time_high(group.front().t, started_, last_th_,
                        [&](uint32_t th) { push<uint64_t>(out, (0x8ull << 28) | th); });
        std::sort(group.begin(), group.end(), [](const EventCD &a, const EventCD &b) {
            return a.p != b.p ? a.p < b.p : a.y != b.y ? a.y < b.y : a.x < b.x;
        });
        const uint32_t ts6 = static_cast<uint32_t>(group.front().t & 0x3F);
        size_t i           = 0;
        while (i < group.size()) {
            const EventCD &first = group[i];
            const uint32_t base  = first.x & ~31u;
            uint32_t valid       = 0;
            for (; i < group.size(); ++i) {
                const EventCD &ev = group[i];
                if (ev.p != first.p || ev.y != first.y || ev.x >= base + 32)
                    break;
                const uint32_t bit = 1u << (ev.x - base);
                if (valid & bit)
                    break; // duplicate pixel: goes into the next word
                valid |= bit;
            }
            const uint32_t header = (static_cast<uint32_t>(first.p ? 0x1 : 0x0) << 28) | (ts6 << 22) |
                                    ((base & 0x7FFu) << 11) | (first.y & 0x7FFu);
            push<uint64_t>(out, header | (static_cast<uint64_t>(valid) << 32));
        }
    }
    // EXT_TRIGGER (legacy layout, low 32 bits): [type 0xA:4][ts:6][unused:9][id:5][unused:7][p:1]
    void encode_trigger_at(timestamp t, int polarity, int id, std::vector<uint8_t> &out) override {
        evt2x_time_high(t, started_, last_th_, [&](uint32_t th) { push<uint64_t>(out, (0x8ull << 28) | th); });
        push<uint64_t>(out, (0xAull << 28) | (static_cast<uint64_t>(t & 0x3F) << 22) | ((id & 0x1Full) << 8) |
                                (polarity & 1ull));
    }

private:
    bool started_      = false;
    timestamp last_th_ = 0;
};

// ------------------------------------------------------------------------------------------------ EVT 3.0
// 16-bit stateful words. Time = TIME_HIGH (12 bits, t >> 12) | TIME_LOW (12 bits). TIME_HIGH is emitted continuously
// (every value, as a sensor does): decoders count time loops on its 4095 -> 0 wraps and the OpenEB robust decoder
// flags discontinuous TIME_HIGH sequences.
// Vectors are emitted as VECT_BASE_X + (VECT_12, VECT_12, VECT_8): 32 pixels, base auto-incremented by 32.
class Evt3Encoder final : public GroupingEncoder {
public:
    Evt3Encoder(int width) : width_(width) {}
    EvtFormat format() const override { return EvtFormat::Evt3; }

protected:
    void encode_group(std::vector<EventCD> &group, std::vector<uint8_t> &out) override {
        encode_time(group.front().t, out);
        std::sort(group.begin(), group.end(), [](const EventCD &a, const EventCD &b) {
            return a.y != b.y ? a.y < b.y : a.p != b.p ? a.p < b.p : a.x < b.x;
        });
        // Exact duplicates (same pixel, polarity and time) cannot share a vector: send them as single events.
        unique_.clear();
        dups_.clear();
        for (size_t i = 0; i < group.size(); ++i) {
            if (i > 0 && group[i].x == group[i - 1].x && group[i].y == group[i - 1].y && group[i].p == group[i - 1].p)
                dups_.push_back(group[i]);
            else
                unique_.push_back(group[i]);
        }
        encode_rows(unique_, true, out);
        encode_rows(dups_, false, out);
    }
    // EXT_TRIGGER: [type 0xA:4][id:4][unused:7][pol:1], at the current TIME_HIGH / TIME_LOW
    void encode_trigger_at(timestamp t, int polarity, int id, std::vector<uint8_t> &out) override {
        encode_time(t, out);
        push<uint16_t>(out, word(0xA, static_cast<uint16_t>(((id & 0xF) << 8) | (polarity & 1))));
    }

private:
    static uint16_t word(uint16_t type, uint16_t content) { return static_cast<uint16_t>((type << 12) | (content & 0xFFF)); }

    void encode_time(timestamp t, std::vector<uint8_t> &out) {
        const timestamp high = t >> 12;
        const int low        = static_cast<int>(t & 0xFFF);
        if (!started_) {
            push<uint16_t>(out, word(0x8, static_cast<uint16_t>(high & 0xFFF)));
            high_    = high;
            started_ = true;
            dec_low_ = -1;
        } else if (high != high_) {
            // Continuous TIME_HIGH, as emitted by a sensor (one word per 4096 us, wrapping 4095 -> 0).
            for (timestamp h = high_ + 1; h <= high; ++h)
                push<uint16_t>(out, word(0x8, static_cast<uint16_t>(h & 0xFFF)));
            high_    = high;
            dec_low_ = 0; // the decoder resets time low when time high changes
        }
        if (low != dec_low_) {
            push<uint16_t>(out, word(0x6, static_cast<uint16_t>(low)));
            dec_low_ = low;
        }
    }

    void encode_rows(const std::vector<EventCD> &evs, bool allow_vectors, std::vector<uint8_t> &out) {
        size_t i = 0;
        while (i < evs.size()) {
            const uint16_t y = evs[i].y;
            const short p    = evs[i].p;
            if (y != dec_y_) {
                push<uint16_t>(out, word(0x0, y & 0x7FF));
                dec_y_ = y;
            }
            size_t j = i;
            while (j < evs.size() && evs[j].y == y && evs[j].p == p)
                ++j;
            encode_run(evs.data() + i, evs.data() + j, p ? 1 : 0, allow_vectors, out);
            i = j;
        }
    }

    // Events of one row and polarity, sorted by x, without duplicates.
    void encode_run(const EventCD *b, const EventCD *e, int pol, bool allow_vectors, std::vector<uint8_t> &out) {
        while (b != e) {
            const int x0         = b->x;
            const bool can_reuse = vbase_valid_ && vpol_ == pol && x0 >= vbase_ && x0 < vbase_ + 32 &&
                                   vbase_ + 32 <= width_;
            const int base = can_reuse ? vbase_ : std::min(x0, std::max(width_ - 32, 0));
            int count      = 0;
            for (auto it = b; it != e && it->x < base + 32; ++it)
                ++count;
            const int vector_cost = 3 + (can_reuse ? 0 : 1);
            if (allow_vectors && count > vector_cost) {
                if (!can_reuse)
                    push<uint16_t>(out, word(0x3, static_cast<uint16_t>((pol << 11) | (base & 0x7FF))));
                uint32_t valid = 0;
                for (int k = 0; k < count; ++k, ++b)
                    valid |= 1u << (b->x - base);
                push<uint16_t>(out, word(0x4, static_cast<uint16_t>(valid & 0xFFF)));
                push<uint16_t>(out, word(0x4, static_cast<uint16_t>((valid >> 12) & 0xFFF)));
                push<uint16_t>(out, word(0x5, static_cast<uint16_t>((valid >> 24) & 0xFF)));
                vbase_       = base + 32;
                vpol_        = pol;
                vbase_valid_ = true;
            } else {
                push<uint16_t>(out, word(0x2, static_cast<uint16_t>((pol << 11) | (x0 & 0x7FF))));
                ++b;
            }
        }
    }

    int width_;
    bool started_   = false;
    timestamp high_ = 0;
    int dec_low_    = -1;
    int dec_y_      = -1;
    int vbase_      = 0;
    int vpol_       = 0;
    bool vbase_valid_ = false;
    std::vector<EventCD> unique_, dups_;
};

} // namespace

std::unique_ptr<EventEncoder> make_encoder(EvtFormat format, int width, int /*height*/) {
    switch (format) {
    case EvtFormat::Evt2:
        return std::make_unique<Evt2Encoder>();
    case EvtFormat::Evt21:
        return std::make_unique<Evt21Encoder>();
    case EvtFormat::Evt3:
        return std::make_unique<Evt3Encoder>(width);
    }
    return nullptr;
}

} // namespace evs
