#pragma once

// Indices into Core's OrderTrace::ext_counters, which has four int32 slots that Core gives no meaning.
// Written on the trace of a BATCH_CLEAR. A voided batch writes zeros except the pivots. The trace of every
// other command has all four at zero. Counts are clamped to int32.
enum class GpuExtSlot {
    LiftedUnits = 0,  // node-hours that landed on a lifted item in this clear
    BidsFilled = 1,   // bids that got at least one unit
    BidsUnfilled = 2, // bids that got nothing
    SolverPivots = 3, // pivots the solver made, or stopped at when it ran out of them
};
