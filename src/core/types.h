#pragma once

#include <cstdint>
#include <metavision/sdk/base/events/event_cd.h>
#include <metavision/sdk/base/events/event_ext_trigger.h>
#include <metavision/sdk/base/utils/timestamp.h>

namespace evs {

using EventCD         = Metavision::EventCD;
using EventExtTrigger = Metavision::EventExtTrigger;
using timestamp = Metavision::timestamp;

// IMX636 / EVK4 geometry, used as default when no source is open.
constexpr int kDefaultWidth  = 1280;
constexpr int kDefaultHeight = 720;

} // namespace evs
