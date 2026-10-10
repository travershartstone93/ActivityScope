#pragma once
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

namespace as {

// A fixed-length ring of samples. A missing sample is stored as NaN so the
// graph can leave a gap instead of drawing a line through it.
class History {
public:
    explicit History(std::size_t capacity = 120) : data_(capacity, gap()) {}

    static double gap() { return std::numeric_limits<double>::quiet_NaN(); }

    void push(double v) {
        data_[head_] = v;
        head_ = (head_ + 1) % data_.size();
        if (count_ < data_.size()) ++count_;
    }
    void push_gap() { push(gap()); }

    std::size_t capacity() const { return data_.size(); }
    std::size_t size() const { return count_; }

    // Oldest first; index 0 is the oldest retained sample.
    double at(std::size_t i) const {
        std::size_t start = (head_ + data_.size() - count_) % data_.size();
        return data_[(start + i) % data_.size()];
    }
    double last() const { return count_ ? at(count_ - 1) : gap(); }

    double max() const {
        double m = 0;
        for (std::size_t i = 0; i < count_; ++i)
            if (!std::isnan(at(i)) && at(i) > m) m = at(i);
        return m;
    }

private:
    std::vector<double> data_;
    std::size_t head_ = 0;
    std::size_t count_ = 0;
};

}  // namespace as
