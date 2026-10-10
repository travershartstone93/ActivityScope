#pragma once
#include <optional>
#include <string>
#include <utility>

namespace as {

// A measured value, or the reason there is none. A missing sensor must never
// turn into a plausible-looking zero.
template <class T>
struct Reading {
    std::optional<T> value;
    std::string why;

    static Reading of(T v) { return {std::move(v), {}}; }
    static Reading none(std::string reason) { return {std::nullopt, std::move(reason)}; }

    bool ok() const { return value.has_value(); }
    T get() const { return *value; }
    T or_else(T fallback) const { return value.value_or(fallback); }
};

}  // namespace as
