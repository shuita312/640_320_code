#pragma once

#include "core/data_bridge.hpp"
#include "core/encode_worker.hpp" // 引入我们全新的无锁、三路解耦单管线共享编码流水线
#include "carina_a1088.h"         // 耀宇底层 SDK

namespace vio {

/**
 * @brief 耀宇 Carina A1088 数据采集类
 * 负责与硬件通信，将两路 RAW 裸图无锁直推外部的共享 EncodeWorker，将 IMU 数据送入 DataBridge。
 */
class OakProvider {
public:
    /**
     * @brief 构造函数
     * @param encoder 传入全局唯一的解耦型共享编码引擎
     */
    explicit OakProvider(EncodeWorker& encoder);
    ~OakProvider();

    // 禁用拷贝和赋值
    OakProvider(const OakProvider&) = delete;
    OakProvider& operator=(const OakProvider&) = delete;

    /**
     * @brief 初始化函数（接收外部控制参数）
     * @param width 外部指定的图像宽度（如 320）
     * @param height 外部指定的图像高度（如 240）
     * @param fps 外部指定的采集帧率（如 30）
     */
    bool initialize(int width, int height, int fps);
    void start();
    void stop();

    // 供静态回调函数获取内部组件的接口
    DataBridge& get_bridge() { return encoder_.get_bridge(); }
    EncodeWorker& get_encoder() { return encoder_; }

private:
    // 硬件编码器接口，直接绑定外部传入
    EncodeWorker& encoder_;   
    
    // 基础配置缓存参数
    int width_;               
    int height_;              
    int fps_;                 
    bool is_running_;
    size_t frame_size_;
        
};

} // namespace vio