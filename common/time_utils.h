#pragma once

#include <chrono>
#include <ctime>
#include <string>
#include <format>
#include <array>

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

#ifdef PERF
/// 返回带纳秒的可读时间字符串，格式：YYYY-MM-DD HH:MM:SS.nnnnnnnnn
inline auto& getCurrentTimeStr(std::string* time_str) {
    using namespace std::chrono;
    auto now = system_clock::now();

    // 拆分秒和纳秒
    auto secs = system_clock::to_time_t(now);
    long nanos = static_cast<long>(duration_cast<nanoseconds>(now.time_since_epoch()).count() % NANOS_TO_SECS);

    // 线程安全转换
    std::tm tm{};
    localtime_r(&secs, &tm);

    // 格式化：{:%Y-%m-%d %H:%M:%S} 来自 <format>
    std::string s = std::format("{:%H:%M:%S}.{:09}", tm, nanos);
    time_str->assign(s);
    return *time_str;
}
#else
/// 返回秒级可读时间字符串，格式：YYYY-MM-DD HH:MM:SS
inline auto& getCurrentTimeStr(std::string* time_str) {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto secs = system_clock::to_time_t(now);

    std::tm tm{};
    localtime_r(&secs, &tm);

    // 不带换行、线程安全
    std::string s = std::format("{:%Y-%m-%d %H:%M:%S}", tm);
    time_str->assign(s);
    return *time_str;
}
#endif
} // namespace Common
