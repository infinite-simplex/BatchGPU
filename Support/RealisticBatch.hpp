#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "ClearingTypes.hpp"
#include "SyntheticBatch.hpp"

// Calibrated to ERCOT's day-ahead order books, a real batch auction over hourly products with stepped
// curve bids. The numbers each constant comes from are noted beside it, and the calibration test checks
// that the output stays inside the ERCOT ranges. What the data fixes, and what it does not:
//
//   Fixed by ERCOT   steps per curve, the heavy tail of order sizes, how often prices tie, how orders
//                    crowd onto a few locations, the share of orders that trade.
//   NOT fixed        how many items a compute order accepts (every ERCOT order names one location),
//                    how orders arrive and are cancelled over time (the files hold only the final
//                    bids), and how compute buyers behave. Those are parameters here, not facts.
//
// Unlike SyntheticBatch.hpp this uses <cmath>, so a batch can differ in the last bit of a price or a
// size between standard libraries. That does not matter for statistics, but a test must not compare
// exact values.
namespace realistic {

struct Params {
    std::uint32_t items{ 250 };
    std::size_t bids{ 1000 };
    double asksPerBid{ 0.74 };          // ERCOT offers per bid: 0.47 to 1.19 over five days, median 0.74
    std::uint32_t offerPerAsk{ 5 };     // compute-specific, not from ERCOT
    std::uint32_t acceptPerBid{ 5 };    // compute-specific, not from ERCOT (ERCOT orders name one location)

    // Steps per curve, clamped at 4. Pooled over five days (bids / offers).
    double bidStepWeights[4]{ 0.659, 0.024, 0.166, 0.152 };
    double askStepWeights[4]{ 0.757, 0.027, 0.029, 0.187 };

    // Order size: lognormal. ERCOT medians are 3.8 to 6.0 MW (bids) and 1.0 to 5.9 (offers);
    // sigma from the p99 is 1.6 to 2.1 and from the p90 is 1.8 to 3.2.
    double bidSizeMedian{ 4.5 };
    double askSizeMedian{ 3.0 };
    double sizeSigma{ 2.0 };
    double maxOrderUnits{ 5000 };       // ERCOT's largest order is about 5,700 MW

    // Prices, in ticks ($0.01). Lognormal around a median, with a share of orders
    // quoting one of a few popular round prices. ERCOT: 51 to 73 percent of
    // orders share their first-step price with another order in the same hour,
    // and 39 to 63 percent of prices in an hour are distinct.
    double bidPriceMedianTicks{ 2500 };
    double askPriceMedianTicks{ 1200 };
    double bidPriceSigma{ 1.7 };
    double askPriceSigma{ 1.2 };
    double clusteredPriceShare{ 0.6 };
    std::uint32_t popularPriceLevels{ 60 };

    // How orders crowd onto a few items. ERCOT fits an exponent of 0.6 to 0.9.
    double zipfExponent{ 0.75 };
};

namespace detail {

inline double uniform01(synthetic::Rng& rng) {
    return static_cast<double>(rng.next() >> 11) * (1.0 / 9007199254740992.0);
}

inline double normal(synthetic::Rng& rng) {
    // Box-Muller. The first uniform is kept away from zero.
    const double u1 = std::max(uniform01(rng), 1e-12);
    const double u2 = uniform01(rng);
    return std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
}

inline std::size_t pickWeighted(synthetic::Rng& rng, const double* weights, std::size_t n) {
    double total = 0;
    for (std::size_t i = 0; i < n; ++i) total += weights[i];
    double x = uniform01(rng) * total;
    for (std::size_t i = 0; i < n; ++i) {
        x -= weights[i];
        if (x < 0) return i;
    }
    return n - 1;
}

// Zipf popularity. The ranking is the same for bids and asks, so demand and supply meet on the same items.
class ItemSampler {
public:
    ItemSampler(std::uint32_t items, double exponent) : cumulative_(items), used_(items, 0) {
        double sum = 0;
        for (std::uint32_t i = 0; i < items; ++i) {
            sum += std::pow(static_cast<double>(i + 1), -exponent);
            cumulative_[i] = sum;
        }
    }

    std::vector<ItemIndex> pick(synthetic::Rng& rng, std::uint32_t count) {
        count = std::min<std::uint32_t>(count, static_cast<std::uint32_t>(cumulative_.size()));
        std::vector<ItemIndex> chosen;
        chosen.reserve(count);
        const double total = cumulative_.back();
        while (chosen.size() < count) {
            const double x = uniform01(rng) * total;
            const auto it = std::lower_bound(cumulative_.begin(), cumulative_.end(), x);
            const ItemIndex item = static_cast<ItemIndex>(it - cumulative_.begin());
            if (item >= used_.size() || used_[item]) continue;
            used_[item] = 1;
            chosen.push_back(item);
        }
        for (ItemIndex item : chosen) used_[item] = 0;
        return chosen;
    }

private:
    std::vector<double> cumulative_;
    std::vector<char> used_;
};

// With probability clusteredPriceShare a first-step price is one of popularPriceLevels round prices
// (popular ones more often), otherwise a fresh lognormal draw.
class PriceModel {
public:
    PriceModel(double medianTicks, double sigma, double clusteredShare, std::uint32_t levels)
        : median_(medianTicks), sigma_(sigma), clusteredShare_(clusteredShare) {
        // Evenly spaced quantiles of the same lognormal, rounded to a multiple of 50 ticks ($0.50)
        // like round numbers.
        for (std::uint32_t i = 0; i < levels; ++i) {
            const double q = (static_cast<double>(i) + 0.5) / static_cast<double>(levels);
            const double z = inverseNormal(q);
            const double value = median_ * std::exp(sigma_ * z);
            levels_.push_back(std::max<double>(50.0, std::round(value / 50.0) * 50.0));
            popularity_.push_back(1.0 / static_cast<double>(i + 1));
        }
    }

    PriceTicks draw(synthetic::Rng& rng) const {
        double ticks;
        if (uniform01(rng) < clusteredShare_) {
            ticks = levels_[pickWeighted(rng, popularity_.data(), popularity_.size())];
        } else {
            ticks = std::round(median_ * std::exp(sigma_ * normal(rng)));
        }
        ticks = std::clamp(ticks, 1.0, static_cast<double>(limits::MAX_PRICE_TICKS));
        return static_cast<PriceTicks>(ticks);
    }

private:
    // Acklam's rational approximation of the inverse normal CDF, good to about 1e-9.
    static double inverseNormal(double p) {
        static const double a[] = { -3.969683028665376e+01, 2.209460984245205e+02, -2.759285104469687e+02,
                                    1.383577518672690e+02, -3.066479806614716e+01, 2.506628277459239e+00 };
        static const double b[] = { -5.447609879822406e+01, 1.615858368580409e+02, -1.556989798598866e+02,
                                    6.680131188771972e+01, -1.328068155288572e+01 };
        static const double c[] = { -7.784894002430293e-03, -3.223964580411365e-01, -2.400758277161838e+00,
                                    -2.549732539343734e+00, 4.374664141464968e+00, 2.938163982698783e+00 };
        static const double d[] = { 7.784695709041462e-03, 3.224671290700398e-01, 2.445134137142996e+00,
                                    3.754408661907416e+00 };
        const double low = 0.02425;
        if (p < low) {
            const double q = std::sqrt(-2 * std::log(p));
            return (((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) /
                   ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1);
        }
        if (p > 1 - low) {
            const double q = std::sqrt(-2 * std::log(1 - p));
            return -(((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) /
                   ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1);
        }
        const double q = p - 0.5;
        const double r = q * q;
        return (((((a[0] * r + a[1]) * r + a[2]) * r + a[3]) * r + a[4]) * r + a[5]) * q /
               (((((b[0] * r + b[1]) * r + b[2]) * r + b[3]) * r + b[4]) * r + 1);
    }

    double median_;
    double sigma_;
    double clusteredShare_;
    std::vector<double> levels_;
    std::vector<double> popularity_;
};

inline ClearingOrder makeOrder(synthetic::Rng& rng, OrderRef id, bool isBid, const Params& p,
                               const PriceModel& prices, ItemSampler& sampler) {
    ClearingOrder order;
    order.id = id;

    const double median = isBid ? p.bidSizeMedian : p.askSizeMedian;
    const double* weights = isBid ? p.bidStepWeights : p.askStepWeights;
    std::size_t steps = pickWeighted(rng, weights, limits::MAX_STEPS) + 1;
    const double drawn = median * std::exp(p.sizeSigma * normal(rng));
    const double limit = std::min<double>(p.maxOrderUnits, static_cast<double>(limits::MAX_ORDER_UNITS));
    const NodeUnits total = static_cast<NodeUnits>(std::clamp(std::round(drawn), 1.0, limit));
    steps = std::min<std::size_t>(steps, total); // every step holds at least one unit
    order.stepCount = static_cast<std::uint8_t>(steps);

    std::vector<double> share(steps);
    double shareSum = 0;
    for (double& s : share) {
        s = 0.2 + uniform01(rng);
        shareSum += s;
    }
    NodeUnits assigned = 0;
    for (std::size_t k = 0; k < steps; ++k) {
        NodeUnits q = (k + 1 == steps)
                          ? total - assigned
                          : std::max<NodeUnits>(1, static_cast<NodeUnits>(std::round(total * share[k] / shareSum)));
        // leave at least one unit for each step still to come
        const NodeUnits room = total - assigned - static_cast<NodeUnits>(steps - k - 1);
        q = std::min(q, room);
        order.steps[k].quantity = q;
        assigned += q;
    }

    double price = prices.draw(rng);
    for (std::size_t k = 0; k < steps; ++k) {
        order.steps[k].price = static_cast<PriceTicks>(std::clamp(std::round(price), isBid ? 1.0 : 0.0,
                                                                  static_cast<double>(limits::MAX_PRICE_TICKS)));
        // a bid's next step is worth less, an ask's costs more
        price = isBid ? price * (0.5 + 0.45 * uniform01(rng)) : price * (1.05 + 0.5 * uniform01(rng));
    }

    // The per-slot cap makes the order spread over its items.
    const std::uint32_t count = isBid ? p.acceptPerBid : std::min<std::uint32_t>(p.offerPerAsk, static_cast<std::uint32_t>(limits::SLOT_WINDOW));
    order.items = sampler.pick(rng, count);
    const double spread = std::max<double>(1.0, static_cast<double>(order.items.size()));
    order.perSlotCap = static_cast<NodeUnits>(std::clamp(std::ceil(2.0 * total / spread), 1.0,
                                                         static_cast<double>(limits::MAX_PER_SLOT_CAP)));
    return order;
}

} // namespace detail

inline ClearingInput makeBatch(const Params& p, std::uint64_t rngSeed) {
    synthetic::Rng rng(rngSeed);
    detail::ItemSampler sampler(p.items, p.zipfExponent);
    const detail::PriceModel bidPrices(p.bidPriceMedianTicks, p.bidPriceSigma, p.clusteredPriceShare, p.popularPriceLevels);
    const detail::PriceModel askPrices(p.askPriceMedianTicks, p.askPriceSigma, p.clusteredPriceShare, p.popularPriceLevels);

    ClearingInput input;
    input.itemCount = p.items;
    const std::size_t asks = static_cast<std::size_t>(std::llround(p.asksPerBid * static_cast<double>(p.bids)));
    for (std::size_t a = 0; a < asks; ++a) {
        input.asks.push_back(detail::makeOrder(rng, 1 + a, false, p, askPrices, sampler));
    }
    for (std::size_t b = 0; b < p.bids; ++b) {
        input.bids.push_back(detail::makeOrder(rng, 1'000'000 + b, true, p, bidPrices, sampler));
    }
    return input;
}

} // namespace realistic
