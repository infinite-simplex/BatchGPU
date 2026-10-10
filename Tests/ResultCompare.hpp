#pragma once
#include <vector>

#include "ClearingTypes.hpp"

// What two replicas running the same input must produce. Timing statistics are not compared.
inline bool sameFills(const std::vector<Fill>& a, const std::vector<Fill>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].order != b[i].order || a[i].item != b[i].item || a[i].units != b[i].units) return false;
    }
    return true;
}

inline bool bitIdentical(const ClearingResult& a, const ClearingResult& b) {
    return a.welfare == b.welfare && a.tieCost == b.tieCost && a.tradedUnits == b.tradedUnits &&
           a.itemUnits == b.itemUnits && a.itemPrice == b.itemPrice && sameFills(a.bidFills, b.bidFills) &&
           sameFills(a.askFills, b.askFills);
}
