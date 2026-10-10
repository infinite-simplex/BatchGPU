#pragma once
#include <cstdint>

#include "BatchCommand.hpp"

inline BatchCommand validBid() {
    BatchCommand c{};
    c.type = CommandType::ORDER;
    c.participantId = 7;
    c.orderToken = 1001;
    c.side = Side::BID;
    c.acceleratorMask = 0b00011; // A100 and H100
    c.interconnectMask = 0b10;   // NVLink
    c.regionMask = 0b001;        // US_WEST
    c.baseHour = 497'000;
    c.slotMask = (std::uint64_t{ 1 } << 10) - 1; // ten hours
    c.perSlotCap = 1;
    c.stepCount = 3;
    c.steps[0] = { 2, 2400 };
    c.steps[1] = { 3, 2200 };
    c.steps[2] = { 5, 2000 };
    return c;
}

inline BatchCommand validAsk() {
    BatchCommand c{};
    c.type = CommandType::ORDER;
    c.participantId = 9;
    c.orderToken = 2002;
    c.side = Side::ASK;
    c.acceleratorMask = 0b00010; // H100
    c.interconnectMask = 0b10;   // NVLink
    c.regionMask = 0b001;        // US_WEST
    c.baseHour = 497'000;
    c.slotMask = 0b1111; // four hours
    c.perSlotCap = 4;
    c.stepCount = 2;
    c.steps[0] = { 8, 1800 };
    c.steps[1] = { 8, 2000 };
    return c;
}
