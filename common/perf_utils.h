#pragma once

#include <cstdint>

namespace Common
{
/// Read from the TSC register and return a uint64_t value representing elapsed CPU clock cycles.
inline auto rdtsc() noexcept {
    unsigned int lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<uint64_t>(hi) << 32) | lo;
}
} // namespace Common

/// Start latency measurement using rdtsc(). Creates a variable called TAG in the local scope.
#define START_MEASURE(TAG) const auto TAG = Common::rdtsc()

/// End latency measurement using rdtsc(). Expects a variable called TAG to already exist in the local scope.
#define END_MEASURE(TAG, LOGGER)                                                                                       \
    do {                                                                                                               \
        const auto end_##TAG = Common::rdtsc();                                                                        \
        LOGGER.log("% RDTSC " #TAG " %\n", Common::getCurrentTimeStr(&time_str_), (end_##TAG - TAG));                  \
    } while (false)

/// Log a current timestamp at the time this macro is invoked.
#define TTT_MEASURE(TAG, LOGGER)                                                                                       \
    do {                                                                                                               \
        const auto TAG = Common::getCurrentNanos();                                                                    \
        LOGGER.log("% TTT " #TAG " %\n", Common::getCurrentTimeStr(&time_str_), TAG);                                  \
    } while (false)
