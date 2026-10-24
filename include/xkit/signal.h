#pragma once

#include <functional>
#include <vector>
#include <utility>

namespace fbx::xkit {

template <typename T>
class Signal {
public:
    using Callback = std::function<void(const T&)>;

    explicit constexpr Signal(T initialValue) noexcept : value_(std::move(initialValue)) {}

    // Observers
    [[nodiscard]] constexpr const T& get() const noexcept {
        return value_;
    }

    // Set a new value and notify observers if it changed
    constexpr void set(T newValue) noexcept {
        if (value_ != newValue) {
            value_ = std::move(newValue);
            notify();
        }
    }

    // Subscribe to changes
    constexpr void subscribe(Callback callback) noexcept {
        subscribers_.push_back(std::move(callback));
    }

private:
    constexpr void notify() const noexcept {
        for (const auto& callback : subscribers_) {
            callback(value_);
        }
    }

    T value_;
    std::vector<Callback> subscribers_;
};

} // namespace fbx::xkit
