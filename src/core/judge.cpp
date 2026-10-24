#include "judge.h"

#include <algorithm>
#include <cmath>

namespace as::judge {

double median(std::vector<double> v) {
    if (v.empty()) return 0;
    std::size_t idx = (v.size() - 1) / 2;
    std::nth_element(v.begin(), v.begin() + long(idx), v.end());
    return v[idx];
}

double mad(const std::vector<double>& v, double med) {
    std::vector<double> dev;
    dev.reserve(v.size());
    for (double x : v) dev.push_back(std::abs(x - med));
    return median(std::move(dev));
}

double percentile(std::vector<double> v, double q) {
    if (v.empty()) return 0;
    std::size_t idx = std::size_t(q * double(v.size() - 1));
    std::nth_element(v.begin(), v.begin() + long(idx), v.end());
    return v[idx];
}

double percentile_rank(const std::vector<double>& pool, double x) {
    if (pool.empty()) return 0;
    double below = 0, ties = 0;
    for (double p : pool) {
        if (p < x) ++below;
        else if (p == x) ++ties;
    }
    return 100.0 * (below + 0.5 * ties) / double(pool.size());
}

double mad_sigma(const std::vector<double>& v) {
    double med = median(v);
    double m = mad(v, med);
    if (m > 0) return 1.4826 * m;
    std::vector<double> c(v);
    double iqr = percentile(c, 0.75) - percentile(c, 0.25);
    return iqr > 0 ? iqr / 1.349 : 0;
}

Trend theil_sen_mk(const std::vector<double>& series) {
    Trend t;
    std::size_t n = series.size();
    if (n < 2) return t;
    std::vector<double> slopes;
    long s = 0;
    for (std::size_t i = 0; i < n; ++i)
        for (std::size_t j = i + 1; j < n; ++j) {
            slopes.push_back((series[j] - series[i]) / double(j - i));
            if (series[j] > series[i]) ++s;
            else if (series[j] < series[i]) --s;
        }
    t.slope_per_step = median(std::move(slopes));
    double var = double(n) * double(n - 1) * double(2 * n + 5) / 18.0;
    if (var > 0 && s != 0) t.mk_z = (double(s) + (s > 0 ? -1 : 1)) / std::sqrt(var);
    t.significant = n >= 10 && std::abs(t.mk_z) >= 1.96;
    return t;
}

Change cusum(const std::vector<double>& series) {
    Change c;
    if (series.size() < 10) return c;
    std::vector<double> base(series.begin(), series.begin() + 7);
    // The centre is the midhinge (midpoint of the quartiles): not moved by an
    // outlier, and unbiased on alternating data where a small baseline's
    // median sits off-centre and reads as a slow drift.
    double med = (percentile(base, 0.25) + percentile(base, 0.75)) / 2;
    double sr = mad_sigma(base);
    if (sr <= 0) return c;
    double k = 0.5 * sr, h = 5 * sr;
    double up = 0, down = 0;
    std::size_t up_start = 0, down_start = 0;
    for (std::size_t i = 0; i < series.size(); ++i) {
        double d = series[i] - med;
        up = std::max(0.0, up + d - k);
        if (up == 0) up_start = i + 1;
        down = std::max(0.0, down - d - k);
        if (down == 0) down_start = i + 1;
        if (up > h || down > h) {
            c.found = true;
            c.index = up > h ? up_start : down_start;
            std::vector<double> after(series.begin() + long(c.index), series.end());
            c.shift_units = std::abs(median(std::move(after)) - med) / sr;
            return c;
        }
    }
    return c;
}

}  // namespace as::judge
