#pragma once

#include <memory>
#include <string>
#include <vector>

#include "core/types.h"

namespace evs {

enum class EvtFormat { Evt2, Evt21, Evt3 };
const char *to_string(EvtFormat format);      // "EVT2", "EVT21", "EVT3"
const char *display_name(EvtFormat format);   // "EVT 2.0", "EVT 2.1", "EVT 3.0"
std::string evt_version(EvtFormat format);    // RAW header "evt" field: "2.0", "2.1", "3.0"
// RAW header "format" field, e.g. "EVT3;height=720;width=1280". EVT 2.1 uses the IMX636 "legacy" word layout.
std::string format_descriptor(EvtFormat format, int width, int height);

// Encodes CD events into a RAW event stream (the byte layout found after the header of a .raw file),
// decodable by the OpenEB decoders.
//
// Events must be passed in non-decreasing timestamp order across calls (an older event is stamped with the latest
// timestamp seen). Events sharing a timestamp are grouped (vectorized for EVT 2.1 / EVT 3) and may be reordered
// within that timestamp, as a sensor would. Timestamps are encoded modulo the format's time range
// (2^34 us for EVT 2.x, 2^24 us for EVT 3.0), with the time-high words needed for decoders to count time loops
// (EVT 3.0 emits every TIME_HIGH value like a sensor: ~490 bytes per second of silence).
// Note: the OpenEB EVT 2.0 decoder detects the 2^34 us loop only in dense streams, so EVT 2.0 files whose
// timestamps exceed ~4.77 h may decode modulo 2^34 us; EVT 2.1 / 3.0 do not have this limitation.
class EventEncoder {
public:
    virtual ~EventEncoder() = default;
    virtual EvtFormat format() const = 0;
    virtual void encode(const EventCD *begin, const EventCD *end, std::vector<uint8_t> &out) = 0;
    // External trigger event (same ordering rule as CD events; id is 5 bits in EVT 2.x, 4 bits in EVT 3.0).
    virtual void encode_trigger(const EventExtTrigger &trigger, std::vector<uint8_t> &out) = 0;
    // Emits the events still held (the last timestamp group).
    virtual void flush(std::vector<uint8_t> &out) = 0;
};

std::unique_ptr<EventEncoder> make_encoder(EvtFormat format, int width, int height);

} // namespace evs
