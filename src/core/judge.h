#pragma once
#include <cstdint>
#include <utility>
#include <vector>

namespace as::judge {

// Deterministic, explainable statistics for the judgment questions. Outlier-proof
// everywhere: medians and MAD, never means, so one spike cannot move the
// baseline it is being judged against. Percentiles use the lower nearest
// rank (idx = q * (n-1), truncated): no interpolation, same answer forever.
//
// Calibrated words, used by every judgment answer:
//   above the 95th percentile          "unusual"   (1 in 20 happens by chance)
//   above the 99th or |zm| > 3.5       "anomalous"
//   Mann-Kendall |z| >= 1.96           "trending"
//   >= 3 MAD units across a date       "changed"

double median(std::vector<double> v);                    // lower median; 0 on empty
double mad(const std::vector<double>& v, double med);    // median absolute deviation
double percentile(std::vector<double> v, double q);      // lower nearest rank
double percentile_rank(const std::vector<double>& pool, double x);  // 0..100
// 1.4826 * MAD, falling back to IQR/1.349 when MAD is 0; 0 means the
// series is essentially constant and no distance can be computed.
double mad_sigma(const std::vector<double>& v);

struct Trend {
    double slope_per_step = 0;  // Theil-Sen: median of pairwise slopes
    double mk_z = 0;            // Mann-Kendall normal approximation
    bool significant = false;   // |z| >= 1.96 and n >= 10
};
Trend theil_sen_mk(const std::vector<double>& series);

struct Change {
    bool found = false;
    std::size_t index = 0;      // first point of the shifted run
    double shift_units = 0;     // |median after - median before| in MAD units
};
// One-sided CUSUM both ways, textbook design k = 0.5 sigma, h = 5 sigma,
// baseline from the first seven points. Needs ten points to say anything.
Change cusum(const std::vector<double>& series);

}  // namespace as::judge
