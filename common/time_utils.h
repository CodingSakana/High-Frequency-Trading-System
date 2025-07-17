#pragma once

#include <chrono>
#include <ctime>
#include <string>

namespace Common
{
typedef int64_t Nanos;

constexpr Nanos NANOS_TO_MICROS = 1000;
constexpr Nanos MICROS_TO_MILLIS = 1000;
constexpr Nanos MILLIS_TO_SECS = 1000;
constexpr Nanos NANOS_TO_MILLIS = NANOS_TO_MICROS * MICROS_TO_MILLIS;
constexpr Nanos NANOS_TO_SECS = NANOS_TO_MILLIS * MILLIS_TO_SECS;

/** 返回当前时间的纳秒数 */
inline auto getCurrentNanos() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/** 返回当前时间的字符串表示，用于人类可读的时间显示 */
inline auto& getCurrentTimeStr(std::string* time_str) {
    const auto time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    /**
     * 等价于
     * time_str->clear();
     * time_str->append(ctime(&time));
     */
    time_str->assign(ctime(&time));
    /* 这里用 .at() 是因为 at() 有边界检查*/
    if (!time_str->empty()) time_str->at(time_str->length() - 1) = '\0';
    return *time_str;
}
} // namespace Common
