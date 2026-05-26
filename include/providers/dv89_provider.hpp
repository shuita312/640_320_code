#pragma once

#include "core/data_bridge.hpp"
#include "core/encode_worker.hpp"
#include <thread>
#include <atomic>
#include <string>
#include <vector>

namespace vio {

class Dv89Provider {
public:
    // 基础配置缓存参数
    int width_;               
    int height_;              
    int fps_;                   
    // 扩展缓冲区深度以应对 SD 卡写入时的毫秒级抖动，确保持续采集不丢帧
    static constexpr int V4L2_BUFFER_COUNT = 8; 

    /**
     * @param encoder 传入共享硬件编码引擎，避免多管线重复申请内存
     */
    Dv89Provider(EncodeWorker& encoder, const std::string& device_path = "/dev/dv89_cam");
    ~Dv89Provider();

    Dv89Provider(const Dv89Provider&) = delete;
    Dv89Provider& operator=(const Dv89Provider&) = delete;

    bool initialize(int width, int height, int fps);
    void start();
    void stop();

private:
    bool init_v4l2();
    void capture_loop(); // 运行于独立 Core 5，负责从 V4L2 泵送物理指针

private:
    EncodeWorker& encoder_; 
    std::string device_path_;
    
    std::thread capture_thread_;
    std::atomic<bool> is_running_;

    // V4L2 物理内存映射控制
    int v4l2_fd_;
    struct Buffer {
        int fd;         // 导出的 DMA-BUF 文件描述符
        void* start;
        size_t length;
    };
    std::vector<Buffer> buffers_;
};

} // namespace vio