#pragma once

#include <cstdint>
#include <time.h>

namespace vio {
namespace time {

/**
 * @brief 获取系统的绝对单调时间 (纳秒 ns)
 * * @note 为什么不使用 std::chrono::steady_clock？
 * steady_clock 在 Linux 底层通常映射为 CLOCK_MONOTONIC。虽然它不会因为用户修改时间而跳变，
 * 但它会受到 NTP (网络时间协议) adjtime() 的平滑微调（Slewing）影响，导致在某段时间内时钟变快或变慢。
 * 对于 400Hz 的 IMU 来说，这种微调引入的误差是致命的。
 * * CLOCK_MONOTONIC_RAW 直接读取基于硬件振荡器的纯粹 Tick 数，彻底免疫 NTP。
 * * @return uint64_t 纳秒级别的时间戳
 */
inline uint64_t get_sys_time_ns() {
    struct timespec ts;
    // 使用 CLOCK_MONOTONIC_RAW 获取最纯粹的硬件级单调时间
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + static_cast<uint64_t>(ts.tv_nsec);
}

/**
 * @brief 获取系统的绝对单调时间 (毫秒 ms)
 * 主要用于给日志打印、终端 UI 监控提供人类易读的时间刻度。
 * * @return uint64_t 毫秒级别的时间戳
 */
inline uint64_t get_sys_time_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000ULL + static_cast<uint64_t>(ts.tv_nsec) / 1000000ULL;
}

/**
 * @brief 用于格式化记录 sync_map.log 的系统时间 (微秒 us)
 * 视频帧往往以微秒级别对齐，可以直接存入外部索引表中。
 * * @return uint64_t 微秒级别的时间戳
 */
inline uint64_t get_sys_time_us() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000ULL + static_cast<uint64_t>(ts.tv_nsec) / 1000ULL;
}

/**
 * @brief 获取系统真实的 UNIX 时间戳 (秒级)
 * * 仅用于创建数据文件夹或日志命名。千万不要用它去给 IMU 或相机帧打时间戳！
 * * @return uint64_t 自 1970 年以来的秒数
 */
inline uint64_t get_unix_timestamp_sec() {
    struct timespec ts;
    // 使用 CLOCK_REALTIME 获取挂钟时间 (UNIX Epoch)
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<uint64_t>(ts.tv_sec);
}

/**
 * @brief 获取系统的真实 UNIX 时间戳 (纳秒 ns)
 * @note 使用 CLOCK_REALTIME，该时间会受系统 NTP 对时、手动改时的影响。
 * 适用于需要与 GPS 或其他主机进行绝对时间对齐的场景。
 * @return uint64_t 自 1970 年以来的纳秒数
 */
inline uint64_t get_unix_time_ns() {
    struct timespec ts;
    // 使用 CLOCK_REALTIME 获取挂钟时间 (UNIX Epoch)
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + static_cast<uint64_t>(ts.tv_nsec);
}



} // namespace time
} // namespace vio