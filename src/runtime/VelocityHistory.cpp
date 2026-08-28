#include "runtime/VelocityHistory.h"

#include <algorithm>
#include <cmath>

namespace rock_wall_climbing
{
    void VelocityHistory::clear() noexcept
    {
        _samples = {};
        _first = 0;
        _count = 0;
        _durationSeconds = 0.0f;
    }

    void VelocityHistory::push(
        const policy::VelocitySample sample,
        const float maximumHistorySeconds) noexcept
    {
        if (!policy::finite(sample.displacement) ||
            !std::isfinite(sample.deltaSeconds) ||
            sample.deltaSeconds <= 0.0f ||
            !std::isfinite(maximumHistorySeconds) ||
            maximumHistorySeconds <= 0.0f) {
            return;
        }

        if (_count == CAPACITY) {
            popOldest();
        }
        const std::size_t index = (_first + _count) % CAPACITY;
        _samples[index] = sample;
        ++_count;
        _durationSeconds += sample.deltaSeconds;

        while (_count > 1 &&
               _durationSeconds > maximumHistorySeconds) {
            popOldest();
        }
    }

    std::size_t VelocityHistory::size() const noexcept
    {
        return _count;
    }

    std::span<const policy::VelocitySample>
    VelocityHistory::copyChronological(
        std::array<policy::VelocitySample, CAPACITY>& output) const noexcept
    {
        for (std::size_t index = 0; index < _count; ++index) {
            output[index] = _samples[(_first + index) % CAPACITY];
        }
        return std::span<const policy::VelocitySample>(
            output.data(),
            _count);
    }

    void VelocityHistory::popOldest() noexcept
    {
        if (_count == 0) {
            return;
        }
        _durationSeconds = std::max(
            0.0f,
            _durationSeconds - _samples[_first].deltaSeconds);
        _samples[_first] = {};
        _first = (_first + 1) % CAPACITY;
        --_count;
    }
}
