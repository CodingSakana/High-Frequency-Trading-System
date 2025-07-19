#pragma once

#include <array>
#include <chrono>
#include <ctime>
#include <cstdio>   // snprintf
#include <string>

namespace Common {

using Nanos = int64_t;

constexpr Nanos NANOS_TO_MICROS = 1000;
constexpr Nanos MICROS_TO_MILLIS = 1000;
constexpr Nanos MILLIS_TO_SECS = 1000;
constexpr Nanos NANOS_TO_MILLIS = NANOS_TO_MICROS * MICROS_TO_MILLIS;
constexpr Nanos NANOS_TO_SECS   = NANOS_TO_MILLIS * MILLIS_TO_SECS;

/** 返回当前时间的纳秒数 */
inline Nanos getCurrentNanos() noexcept {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(
               system_clock::now().time_since_epoch()
           ).count();
}

#ifdef PERF
// 输出格式：HH:MM:SS.nnnnnnnnn
inline std::string& getCurrentTimeStr(std::string* time_str) noexcept {
    using namespace std::chrono;

    const auto  now      = system_clock::now();
    const auto  now_secs = floor<seconds>(now);              // 截到秒
    const auto  nanos    = duration_cast<nanoseconds>(
                               now.time_since_epoch()
                           ).count() % NANOS_TO_SECS;

    const time_t tt = system_clock::to_time_t(now_secs);
    std::tm      tm {};
    localtime_r(&tt, &tm);                                   // 线程安全

    char hhmmss[9];                                          // "HH:MM:SS"
    std::strftime(hhmmss, sizeof(hhmmss), "%H:%M:%S", &tm);

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%s.%09ld", hhmmss, nanos);

    time_str->assign(buf);
    return *time_str;
}
#else
// 输出格式：YYYY-MM-DD HH:MM:SS
inline std::string& getCurrentTimeStr(std::string* time_str) noexcept {
    using namespace std::chrono;

    const auto now  = system_clock::now();
    const time_t tt = system_clock::to_time_t(now);

    std::tm tm {};
    localtime_r(&tt, &tm);                                   // 线程安全

    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);

    time_str->assign(buf);
    return *time_str;
}
#endif

} // namespace Common
