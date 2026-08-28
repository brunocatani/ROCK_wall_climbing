#pragma once

#include "runtime/ClimbingPolicy.h"

#include <array>
#include <cstddef>
#include <span>

namespace rock_wall_climbing
{
    class VelocityHistory
    {
    public:
        static constexpr std::size_t CAPACITY = 24;

        void clear() noexcept;
        void push(
            policy::VelocitySample sample,
            float maximumHistorySeconds) noexcept;

        [[nodiscard]] std::size_t size() const noexcept;
        [[nodiscard]] std::span<const policy::VelocitySample>
            copyChronological(
                std::array<policy::VelocitySample, CAPACITY>& output)
                const noexcept;

    private:
        void popOldest() noexcept;

        std::array<policy::VelocitySample, CAPACITY> _samples{};
        std::size_t _first{ 0 };
        std::size_t _count{ 0 };
        float _durationSeconds{ 0.0f };
    };
}
