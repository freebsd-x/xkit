#pragma once

#include "xkit/signal.h"
#include "xkit/layout.h"
#include <functional>
#include <vector>
#include <memory>
#include <algorithm>

namespace fbx::xkit {

// --- 1. Mathematical Interpolation (Lerp) ---

template<typename T>
struct Lerp {
    static constexpr T compute(const T& a, const T& b, float t) noexcept {
        return a + static_cast<T>(static_cast<float>(b - a) * t);
    }
};

template<>
struct Lerp<Offset> {
    static constexpr Offset compute(const Offset& a, const Offset& b, float t) noexcept {
        return {
            a.x_ + (b.x_ - a.x_) * t,
            a.y_ + (b.y_ - a.y_) * t
        };
    }
};

template<>
struct Lerp<Size> {
    static constexpr Size compute(const Size& a, const Size& b, float t) noexcept {
        return {
            a.width_ + (b.width_ - a.width_) * t,
            a.height_ + (b.height_ - a.height_) * t
        };
    }
};

// --- 2. The Ticker System ---

class Ticker {
public:
    using TickCallback = std::function<bool(float dt)>; // Return true if finished
    
    void addAnimation(TickCallback cb) noexcept {
        animations_.push_back(std::move(cb));
    }
    
    void tick(float dt) noexcept {
        for (auto it = animations_.begin(); it != animations_.end(); ) {
            bool finished = (*it)(dt);
            if (finished) {
                it = animations_.erase(it);
            } else {
                ++it;
            }
        }
    }

    [[nodiscard]] bool hasActiveAnimations() const noexcept {
        return !animations_.empty();
    }

private:
    std::vector<TickCallback> animations_;
};

// Damped Harmonic Oscillator for Flutter/iOS style spring physics
class SpringSimulation {
public:
    SpringSimulation(float mass = 1.0f, float stiffness = 100.0f, float damping = 10.0f) noexcept
        : mass_(mass), stiffness_(stiffness), damping_(damping) {}

    float step(float dt, float current_val, float target_val, float& velocity) const noexcept {
        // F = -k*x - c*v
        float displacement = current_val - target_val;
        float spring_force = -stiffness_ * displacement;
        float damping_force = -damping_ * velocity;
        float acceleration = (spring_force + damping_force) / mass_;
        
        velocity += acceleration * dt;
        return current_val + velocity * dt;
    }

    bool isAtRest(float current_val, float target_val, float velocity) const noexcept {
        return std::abs(current_val - target_val) < 0.01f && std::abs(velocity) < 0.01f;
    }

private:
    float mass_;
    float stiffness_;
    float damping_;
};

// --- 3. The AnimatedSignal ---

template<typename T>
class AnimatedSignal : public Signal<T> {
public:
    AnimatedSignal(T initial, std::shared_ptr<Ticker> ticker) noexcept 
        : Signal<T>(std::move(initial)), ticker_(std::move(ticker)) {}

    void animateTo(T target, float duration_sec) noexcept {
        T start = this->get();
        float elapsed = 0.0f;
        
        ticker_->addAnimation([this, start, target, duration_sec, elapsed](float dt) mutable -> bool {
            elapsed += dt;
            float t = std::min(elapsed / duration_sec, 1.0f);
            
            // Basic Quadratic Ease-Out interpolation
            float easeOut = 1.0f - (1.0f - t) * (1.0f - t);

            this->set(Lerp<T>::compute(start, target, easeOut));
            
            return t >= 1.0f; // Return true to remove from Ticker
        });
    }

    void springTo(T target, SpringSimulation spring = SpringSimulation()) noexcept {
        float velocity = 0.0f;
        
        ticker_->addAnimation([this, target, spring, velocity](float dt) mutable -> bool {
            float current_val = static_cast<float>(this->get());
            float next_val = spring.step(dt, current_val, static_cast<float>(target), velocity);
            
            this->set(static_cast<T>(next_val));
            
            if (spring.isAtRest(next_val, static_cast<float>(target), velocity)) {
                this->set(target);
                return true;
            }
            return false;
        });
    }

private:
    std::shared_ptr<Ticker> ticker_;
};

} // namespace fbx::xkit
