#pragma once
#include <cstdint>

// 1 tick = $0.01 per node-hour.
using PriceTicks = std::uint32_t;

// Node-hours: one node for one hour. There are no sub-node trades.
using NodeUnits = std::uint32_t;

// One SKU in one hour of the batch's window, numbered hour * SKU_COUNT + SKU (see itemIndex in
// Catalog.hpp). The clearing module accepts any index below its item count.
using ItemIndex = std::uint32_t;

// Unique within its side of a batch. The engine uses the order's arrival rank.
using OrderRef = std::uint64_t;

// Absolute UTC hours since the epoch, never relative to a clear.
using HourIndex = std::uint32_t;

// Asserted by the client. Nothing authenticates it.
using ParticipantId = std::uint32_t;

// The client's own name for an order, used to cancel it and to match its results.
using OrderToken = std::uint64_t;

// Not checked against earlier ids: any BATCH_CLEAR is accepted.
using BatchId = std::uint64_t;
