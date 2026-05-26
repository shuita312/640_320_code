#pragma once

#include "core/data_bridge.hpp"
#include <thread>
#include <atomic>
#include <string>
#include <fstream>
#include <vector>
#include <iomanip>

// 向前声明 FFmpeg 结构体
extern "C" {
struct AVFormatContext;
struct AVStream;
struct AVPacket; // AVPacket 的向前声明
}

namespace vio {

class StorageWorker {
public:
    /**
     * @brief 构造函数
     * @param bridge 全局数据缓冲大动脉桥接器
     * @param output_dir 数据分段写盘目标存储路径
     */
    StorageWorker(DataBridge& bridge, const std::string& output_dir);
    ~StorageWorker();

    StorageWorker(const StorageWorker&) = delete;
    StorageWorker& operator=(const StorageWorker&) = delete;

    /**
     * @brief 执行底层缓冲区配置及首期磁盘文件分配点亮
     */
    bool initialize();
    void start();
    void stop();

private:
    void process_loop();
    bool open_files();
    void close_files();
    void write_packet_to_disk(const DataPacket& pkt);
    void rotate_files(uint64_t current_ts_ns);
    bool init_mp4_muxer(AVFormatContext*& ctx, AVStream*& stream, const std::string& filename, int width, int height);
    
    // 高性能 H.264 Annex-B 码流 SPS/PPS 动态提取器（保留以支持潜在扩展，当前直通已不强依赖）
    bool extract_sps_pps(const uint8_t* data, size_t size, std::vector<uint8_t>& extradata);

private:
    DataBridge& data_bridge_;      
    std::string output_dir_;       

    std::thread worker_thread_;    
    std::atomic<bool> is_running_; 

    // --- FFmpeg 实时 MP4 封装句柄 ---
    AVFormatContext* fmt_ctx_oak_left_  = nullptr;
    AVFormatContext* fmt_ctx_oak_right_ = nullptr;
    AVFormatContext* fmt_ctx_dv89_      = nullptr;

    AVStream* stream_oak_left_  = nullptr;
    AVStream* stream_oak_right_ = nullptr;
    AVStream* stream_dv89_      = nullptr;

    // 全局复用的 AVPacket，根除每帧分配内存带来的碎片化问题
    AVPacket* shared_av_pkt_    = nullptr;

    // 状态一致性：三路视频流 MP4 头部写入状态原子锁，防范未写头先写帧的乱序崩盘
    bool header_written_oak_left_  = false;
    bool header_written_oak_right_ = false;
    bool header_written_dv89_      = false;

    // --- 文本与原始高频二进制文件流描述符 ---
    std::ofstream file_imu_bin_;
    std::ofstream file_sync_map_;

    int chunk_index_ = 0;                                                           // 切片序号，从 0 开始单调递增
    uint64_t current_chunk_start_ns_ = 0;                                           // 当前切片的系统绝对纳秒基准

    // 终极适配点：彻底补齐属于三路完全解耦流的局部切片相对参考时间轴
    uint64_t chunk_start_dev_ts_left_  = 0;                                         
    uint64_t chunk_start_dev_ts_right_ = 0;                                         
    uint64_t chunk_start_dev_ts_dv89_  = 0;                                         // ── 完美解决 .cpp 报错缺失数轴变量 ──

    static constexpr uint64_t CHUNK_DURATION_NS = 10ULL * 60ULL * 1000000000ULL;    // 10分钟强行滚动切片时长，单位：纳秒
    static constexpr size_t BUFFER_SIZE_IMU  = 16 * 1024 * 1024;                    // 16MB 高爆发大容量物理落盘 IMU 环形硬缓冲 
    static constexpr size_t BUFFER_SIZE_SYNC = 16 * 1024 * 1024;                    // 16MB 映射对齐文本专属硬缓冲

    std::vector<char> buffer_imu_;
    std::vector<char> buffer_sync_;
};

} // namespace vio