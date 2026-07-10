#pragma once

namespace spky {

// Fixed target slots. The five pads keep the same function class in both
// engines (spec "Fixed target slots"). Lane index == pad slot == target slot.
enum LaneId {
    LANE_SOURCE = 0,   // Pad 1: POSITION / TIMBRE   (rate x2)
    LANE_SIZE   = 1,   // Pad 2: SIZE / FILTER       (rate x1/2)
    LANE_PITCH  = 2,   // Pad 3: PITCH (master lane) (rate x1)
    LANE_MOTION = 3,   // Pad 4: SHAPE / MOTION      (rate x3/4)
    LANE_LEVEL  = 4,   // Pad 5: LEVEL               (rate x3/2)
    LANE_COUNT  = 5
};

enum class SyncMode { Sync, SyncTriplet, Free };

} // namespace spky
