#include "BatchClearing.hpp"

#include "ClearingImpl.hpp"
#include "MinCostFlow.hpp"
#include "NetworkSimplex.hpp"

ClearingResult clear(const ClearingInput& input) {
    return clear(input, ClearingOptions{});
}

ClearingResult clear(const ClearingInput& input, const ClearingOptions& options) {
    return clearWith<NetworkSimplex>(input, options);
}

ClearingResult clearReference(const ClearingInput& input) {
    return clearWith<MinCostFlow>(input);
}
