#pragma once

#include <cstdint>
#include <vector>
#include <utility>
#include <cstddef>

namespace vio {

// 定义数据包的类型枚举 (使用 class enum 保证类型安全)

enum class DataType : uint8_t {
    OAK_H264_LEFT = 0,  // OAK-D 左目 H.264 视频流
    OAK_H264_RIGHT,     // OAK-D 右目 H.264 视频流
    DV89_H264,          // DV89 辅助摄像头 H.264 视频流
    IMU_RAW,            // OAK-D IMU 原始数据流
    SYS_CMD             // 预留：系统控制命令包
};

// 统一数据载体
struct DataPacket {
    DataType type;
    uint64_t sys_ts;  // 系统单调时钟时间戳 (纳秒 ns)，用于多路异构对齐
    uint64_t dev_ts;  // 硬件内部时间戳 (纳秒 ns)，用于 VIO 紧耦合计算
    
    // 数据载荷
    // 对于视频流，这里存放的是 H.264 的 NALU 单元
    // 对于 IMU，存放的是序列化后的传感器数据 (如 6 个 float)
    std::vector<uint8_t> payload;

    bool is_keyframe = false; // 仅对视频帧有效，标记是否为关键帧 (IDR)

    // 1. 默认构造函数
    DataPacket() : type(DataType::SYS_CMD), sys_ts(0), dev_ts(0) {}

    // 2. 带参构造函数 (支持预分配内存大小)
    DataPacket(DataType t, uint64_t sys, uint64_t dev, size_t reserve_size = 0)
        : type(t), sys_ts(sys), dev_ts(dev) {
        if (reserve_size > 0) {
            // 预分配内存，减少 std::vector 在 append 数据时的 realloc 开销
            payload.reserve(reserve_size); 
        }
    }

    // ----------------------------------------------------------------------
    // 性能优化核心区：零拷贝传递
    // ----------------------------------------------------------------------

    // 3. 移动构造函数
    // 保证在 push 进无锁队列时，直接接管原有内存的所有权
    DataPacket(DataPacket&& other) noexcept 
        : type(other.type), 
          sys_ts(other.sys_ts), 
          dev_ts(other.dev_ts), 
          payload(std::move(other.payload)) {
        // 移动后，将 other 的状态重置，防止悬挂
        other.sys_ts = 0;
        other.dev_ts = 0;
        other.is_keyframe = false;
    }

    // 4. 移动赋值运算符
    DataPacket& operator=(DataPacket&& other) noexcept {
        if (this != &other) {
            type = other.type;
            sys_ts = other.sys_ts;
            dev_ts = other.dev_ts;
            payload = std::move(other.payload);
            
            other.sys_ts = 0;
            other.dev_ts = 0;
        }
        return *this;
    }

    // 5. 禁用拷贝构造和拷贝赋值 (关键防御机制)
    // 编译器会报错，防止开发者手滑写出类似 `DataPacket p2 = p1;` 的代码，
    // 从而避免在主循环中发生巨大的 H.264 内存拷贝卡顿。
    // 在入队和出队时，必须使用 std::move()。
    DataPacket(const DataPacket&) = delete;
    DataPacket& operator=(const DataPacket&) = delete;
};

// ----------------------------------------------------------------------
// 辅助结构体定义
// ----------------------------------------------------------------------

// 针对 IMU 这种高频固定大小的包，定义一个强类型结构。
// 在实际推入 payload 时，可以使用 `reinterpret_cast` 或 `memcpy` 快速拷贝进去。
// 同样，落盘或离线解析时，也可以直接映射出来。
#pragma pack(push, 1) // 取消内存对齐，保证结构体紧凑，方便直接写入 .bin 文件
struct ImuRawData {
    float accel_x;
    float accel_y;
    float accel_z;
    float gyro_x;
    float gyro_y;
    float gyro_z;
};
#pragma pack(pop)

} // namespace vio