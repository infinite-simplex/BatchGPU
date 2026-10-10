#pragma once
#include "ClearingOptions.hpp"
#include "ClearingTypes.hpp"

// Maximizes welfare (bid value minus ask cost); among those allocations trades the most units; among
// those favors earlier orders. Prices are the minimal optimal price vector. Identical input, including
// the order of the order vectors and any explicit ranks, gives an identical result.
//
// Welfare, volume, the rank sum and the price of every item that trades are properties of the input.
// Which of several equally good allocations comes out is decided by the solver's pivot rule, so every
// replica must run the same solver and a change to it is a change to the protocol. The tests pin the
// output of fixed batches to catch an accidental one.
//
// Throws std::invalid_argument for input the handler should have refused (including more arcs than
// ClearingOptions::maxArcs), ClearingBudgetExceeded when the pivot budget runs out, and std::logic_error
// when the solver's own optimality check fails, which is a bug and returns no result.
ClearingResult clear(const ClearingInput& input);
ClearingResult clear(const ClearingInput& input, const ClearingOptions& options);

// The same problem solved by a different and much slower method (successive shortest paths), as an
// independent implementation to compare against. Not meant to run in the engine: at 10,000 bids it
// takes from 11 s to several minutes.
ClearingResult clearReference(const ClearingInput& input);
