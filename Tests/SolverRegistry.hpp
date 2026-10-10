#pragma once
#include <vector>

#include "BatchClearing.hpp"

// A test that covers every way of running the clear loops over allSolvers() and names the solver with
// SCOPED_TRACE, so that a failure says which one broke.
struct SolverUnderTest {
    const char* name;
    ClearingResult (*clear)(const ClearingInput&);
};

inline ClearingResult clearWithSelfCheck(const ClearingInput& input) {
    ClearingOptions options;
    options.selfCheck = true;
    return clear(input, options);
}

inline const std::vector<SolverUnderTest>& allSolvers() {
    static const std::vector<SolverUnderTest> solvers = {
        { "reference_ssp", &clearReference },
        { "network_simplex", static_cast<ClearingResult (*)(const ClearingInput&)>(&clear) },
        { "network_simplex_self_check", &clearWithSelfCheck },
    };
    return solvers;
}
