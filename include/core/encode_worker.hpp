#pragma once

#include "core/data_bridge.hpp"
#include <thread>
#include <atomic>
#include <vector>
#include <cstdint>
#include <string>
#include <mutex>              
#include <condition_variable> 

// 引入第三方顶级无锁队列组件
#include "concurrentqueue.h"

// GStreamer 核心组件
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>

#define USE_HARDWARE_ENCODER 1

namespace vio {

enum class ChannelType : uint8_t {
    LEFT_EYE  = 0,
    RIGHT_EYE = 1,
    DV89_AUX  = 2
};

struct RawImagePacket {
    ChannelType channel = ChannelType::LEFT_EYE;
    uint64_t sys_ts = 0;
    uint64_t dev_ts = 0;
    std::vector<uint8_t> payload;

    // 零拷贝模式 (DMA-BUF) ---
    int dmabuf_fd = -1;  // V4L2导出的硬件内存文件描述符 (有效值通常 >= 0)
    size_t size = 0;     // 硬件缓冲区中的有效数据大小

};

class EncodeWorker {
public:
    explicit EncodeWorker(DataBridge& bridge, int width =640, int height = 320, int fps = 30);
    ~EncodeWorker();

    EncodeWorker(const EncodeWorker&) = delete;
    EncodeWorker& operator=(const EncodeWorker&) = delete;

    bool start();
    void stop();
     
    // 物理内存零拷贝直推接口，接收 DMA-BUF FD (包含 OAK 的池化 FD 和 DV89 的导出 FD)
    void push_dmabuf_left(uint64_t sys_ts, uint64_t dev_ts, int dmabuf_fd, size_t size);
    void push_dmabuf_right(uint64_t sys_ts, uint64_t dev_ts, int dmabuf_fd, size_t size);
    void push_dmabuf_dv89(uint64_t sys_ts, uint64_t dev_ts, int dmabuf_fd, size_t size);

    // 原始数据推送接口
    void push_raw_left(uint64_t sys_ts, uint64_t dev_ts, const uint8_t* data, size_t size);
    void push_raw_right(uint64_t sys_ts, uint64_t dev_ts, const uint8_t* data, size_t size);
    void push_raw_dv89(uint64_t sys_ts, uint64_t dev_ts, const uint8_t* data, size_t size);
    
    DataBridge& get_bridge() { return data_bridge_; }

private:
    // 三路完全解耦的硬件回调静态路由
    static GstFlowReturn on_left_sample(GstAppSink* sink, gpointer user_data);
    static GstFlowReturn on_right_sample(GstAppSink* sink, gpointer user_data);
    static GstFlowReturn on_dv89_sample(GstAppSink* sink, gpointer user_data);
    
    // 三路独立隔离的并发驱动线程
    void left_drive_loop();
    void right_drive_loop();
    void dv89_drive_loop();

    // 独立管线安全构建工厂
    bool build_single_pipeline(GstElement*& pipeline, GstElement*& appsrc, GstElement*& appsink, 
                               const std::string& pipe_name, const std::string& src_name, 
                               const std::string& sink_name, GstAppSinkCallbacks& cb);

private:
    DataBridge& data_bridge_;
    int width_;
    int height_;
    int fps_;
    std::atomic<bool> is_running_;

    // 各路专属的独立单调递增时轴计数器
    uint64_t left_frame_count_ = 0;
    uint64_t right_frame_count_ = 0;
    uint64_t dv89_frame_count_ = 0;

    // 三路独立的无锁并发大队列
    moodycamel::ConcurrentQueue<RawImagePacket> left_queue_;
    std::mutex left_mtx_; std::condition_variable left_cv_; std::thread left_thread_;

    moodycamel::ConcurrentQueue<RawImagePacket> right_queue_;
    std::mutex right_mtx_; std::condition_variable right_cv_; std::thread right_thread_;

    moodycamel::ConcurrentQueue<RawImagePacket> dv89_queue_;
    std::mutex dv89_mtx_; std::condition_variable dv89_cv_; std::thread dv89_thread_;

    // ── 三个物理对等的独立硬件编码管线资源 ──
    GstElement* pipeline_l_ = nullptr; GstElement* appsrc_l_ = nullptr; GstElement* appsink_l_ = nullptr;
    GstElement* pipeline_r_ = nullptr; GstElement* appsrc_r_ = nullptr; GstElement* appsink_r_ = nullptr;
    GstElement* pipeline_d_ = nullptr; GstElement* appsrc_d_ = nullptr; GstElement* appsink_d_ = nullptr;
};

} // namespace vio