#include "storage/storage_worker.hpp"
#include "utils/logger.hpp"
#include "utils/time_sync.hpp"
#include <filesystem>
#include <chrono>
#include <sstream>
#include <cstring>
#include <iomanip>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
}

// =====================================================================
// 1. 匿名空间 (文件内部私有辅助组件，不污染全局符号)
// =====================================================================
namespace {

static std::string get_chunk_suffix(int index) {
    std::ostringstream ss;
    ss << "_" << std::setfill('0') << std::setw(4) << index;
    return ss.str();
}

} // namespace

// =====================================================================
// 2. 正式属于 vio 命名空间的具体类函数实现 
// =====================================================================
namespace vio {

StorageWorker::StorageWorker(DataBridge& bridge, const std::string& output_dir)
    : data_bridge_(bridge), output_dir_(output_dir), is_running_(false) {
    if (!output_dir_.empty() && output_dir_.back() != '/') {
        output_dir_ += '/';
    }
}

StorageWorker::~StorageWorker() {
    stop();
    // 销毁复用的 AVPacket，确保无内存泄漏
    if (shared_av_pkt_) {
        av_packet_free(&shared_av_pkt_);
        shared_av_pkt_ = nullptr;
    }
}

bool StorageWorker::initialize() {
    VIO_LOG_INFO("[Storage Worker] 初始化...");
    try {
        buffer_imu_.resize(BUFFER_SIZE_IMU);
        buffer_sync_.resize(BUFFER_SIZE_SYNC);
    } catch (const std::bad_alloc& e) {
        VIO_LOG_ERROR("[Storage Worker] 无法分配 IO 缓冲区! RAM 已满。错误: " << e.what());
        return false;
    }

    // 伴随 Worker 生命周期分配一次 AVPacket
    shared_av_pkt_ = av_packet_alloc();
    if (!shared_av_pkt_) {
        VIO_LOG_ERROR("[Storage Worker] 无法分配 AVPacket!");
        return false;
    }

    std::error_code ec;
    if (!std::filesystem::exists(output_dir_) && !std::filesystem::create_directories(output_dir_, ec)) {
        VIO_LOG_ERROR("[Storage Worker] 无法创建输出目录: " << output_dir_ << " (" << ec.message() << ")");
        return false;
    }

    return open_files();
}

bool StorageWorker::init_mp4_muxer(AVFormatContext*& ctx, AVStream*& stream, const std::string& filename, int width, int height) {
    std::string full_path = output_dir_ + filename;
    
    if (avformat_alloc_output_context2(&ctx, nullptr, "mp4", full_path.c_str()) < 0) {
        VIO_LOG_ERROR("[Storage Worker] 无法为 " << filename << " 分配 MP4 上下文");
        return false;
    }

    stream = avformat_new_stream(ctx, nullptr);
    if (!stream) {
        VIO_LOG_ERROR("[Storage Worker] 无法创建视频流 for " << filename);
        return false;
    }

    // ── 核心参数设置 ──
    stream->codecpar->codec_id = AV_CODEC_ID_H264;
    stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    stream->codecpar->width = width;
    stream->codecpar->height = height;
    stream->codecpar->format = AV_PIX_FMT_YUV420P;
    
    // 开启 Annex-B 起始码物理直通
    stream->codecpar->codec_tag = 0; 
    
    // 强制将分段容器内部的时轴底座换算设定为高精度纳秒刻度
    stream->time_base = {1, 1000000000}; 

    if (!(ctx->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&ctx->pb, full_path.c_str(), AVIO_FLAG_WRITE) < 0) {
            VIO_LOG_ERROR("FFmpeg: Could not open file via avio: " << full_path);
            return false;
        }
    }

    return true;
}

bool StorageWorker::open_files() {
    std::string suffix = get_chunk_suffix(chunk_index_);

    header_written_oak_left_  = false;
    header_written_oak_right_ = false;
    header_written_dv89_      = false;

    if (!init_mp4_muxer(fmt_ctx_oak_left_,  stream_oak_left_,  "oak_stereo_left"  + suffix + ".mp4", 320, 240)  ||
        !init_mp4_muxer(fmt_ctx_oak_right_, stream_oak_right_, "oak_stereo_right" + suffix + ".mp4", 320, 240)  ||
        !init_mp4_muxer(fmt_ctx_dv89_,      stream_dv89_,      "dv89_aux"         + suffix + ".mp4", 320, 240)) {
        return false;
    }

    file_imu_bin_.rdbuf()->pubsetbuf(buffer_imu_.data(), buffer_imu_.size());
    file_imu_bin_.open(output_dir_ + "imu_raw" + suffix + ".bin", std::ios::binary | std::ios::out | std::ios::trunc);

    file_sync_map_.rdbuf()->pubsetbuf(buffer_sync_.data(), buffer_sync_.size());
    file_sync_map_.open(output_dir_ + "sync_map" + suffix + ".log", std::ios::out | std::ios::trunc);
    file_sync_map_ << "# Sync Map Chunk " << chunk_index_ << ": format = [sys_ts_ns, dev_ts_ns, packet_type, payload_size]\n";

    if (!file_imu_bin_.is_open() || !file_sync_map_.is_open()) {
        VIO_LOG_ERROR("[Storage Worker] 无法打开 chunk " << chunk_index_ << " 的二进制/日志文件");
        return false;
    }

    VIO_LOG_INFO("[Storage Worker] >>>> [成功] 三通道实时分段 MP4 存储组 " << chunk_index_ << " 已圆满拉通。");
    return true;
}

void StorageWorker::close_files() {
    if (fmt_ctx_oak_left_) {
        if (header_written_oak_left_) av_write_trailer(fmt_ctx_oak_left_);
        avio_closep(&fmt_ctx_oak_left_->pb);
        avformat_free_context(fmt_ctx_oak_left_);
        fmt_ctx_oak_left_ = nullptr;
    }
    if (fmt_ctx_oak_right_) {
        if (header_written_oak_right_) av_write_trailer(fmt_ctx_oak_right_);
        avio_closep(&fmt_ctx_oak_right_->pb);
        avformat_free_context(fmt_ctx_oak_right_);
        fmt_ctx_oak_right_ = nullptr;
    }
    if (fmt_ctx_dv89_) {
        if (header_written_dv89_) av_write_trailer(fmt_ctx_dv89_);
        avio_closep(&fmt_ctx_dv89_->pb);
        avformat_free_context(fmt_ctx_dv89_);
        fmt_ctx_dv89_ = nullptr;
    }
    if (file_imu_bin_.is_open())   { file_imu_bin_.flush();   file_imu_bin_.close(); }
    if (file_sync_map_.is_open())  { file_sync_map_.flush();  file_sync_map_.close(); }
}

void StorageWorker::rotate_files(uint64_t current_ts_ns) {
    close_files();
    chunk_index_++;
    current_chunk_start_ns_ = current_ts_ns;
    
    chunk_start_dev_ts_left_  = 0;
    chunk_start_dev_ts_right_ = 0;
    chunk_start_dev_ts_dv89_  = 0;
    
    open_files();
}

void StorageWorker::write_packet_to_disk(const DataPacket& pkt) {
    auto now_clock = std::chrono::system_clock::now().time_since_epoch();
    uint64_t real_unix_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now_clock).count();
    
    if (pkt.type == DataType::OAK_H264_LEFT || pkt.type == DataType::OAK_H264_RIGHT || pkt.type == DataType::DV89_H264) {
        if (pkt.payload.empty() || pkt.payload.size() <= 4) return; 

        AVFormatContext* ctx = nullptr;
        AVStream* stream = nullptr;
        bool* is_header_done = nullptr;
        uint64_t pts_ns = 0;

        if (pkt.type == DataType::OAK_H264_LEFT) {
            ctx = fmt_ctx_oak_left_; stream = stream_oak_left_; is_header_done = &header_written_oak_left_;
            if (chunk_start_dev_ts_left_ == 0) chunk_start_dev_ts_left_ = pkt.sys_ts;
            pts_ns = pkt.sys_ts - chunk_start_dev_ts_left_;
        } 
        else if (pkt.type == DataType::OAK_H264_RIGHT) {
            ctx = fmt_ctx_oak_right_; stream = stream_oak_right_; is_header_done = &header_written_oak_right_;
            if (chunk_start_dev_ts_right_ == 0) chunk_start_dev_ts_right_ = pkt.sys_ts;
            pts_ns = pkt.sys_ts - chunk_start_dev_ts_right_;
        } 
        else if (pkt.type == DataType::DV89_H264) {
            ctx = fmt_ctx_dv89_; stream = stream_dv89_; is_header_done = &header_written_dv89_;
            if (chunk_start_dev_ts_dv89_ == 0) chunk_start_dev_ts_dv89_ = pkt.sys_ts;
            pts_ns = pkt.sys_ts - chunk_start_dev_ts_dv89_;
        }

        if (ctx && stream && is_header_done) {
            if (!(*is_header_done)) {
                if (avformat_write_header(ctx, nullptr) >= 0) {
                    *is_header_done = true;
                    VIO_LOG_INFO("[Storage Worker] MP4 骨架建立并成功写头。类型标签: [" << static_cast<int>(pkt.type) << "]");
                } else {
                    VIO_LOG_ERROR("[Storage Worker] FFmpeg: 为流 " << static_cast<int>(pkt.type) << " 写入 MP4 头失败");
                    return;
                }
            }

            // 重置复用的包状态，安全装填新的有效负载
            av_packet_unref(shared_av_pkt_); 
            shared_av_pkt_->data = const_cast<uint8_t*>(pkt.payload.data());
            shared_av_pkt_->size = pkt.payload.size();

            // 转换绝对单调纳秒为 MP4 TimeBase
            AVRational in_time_base = {1, 1000000000};
            shared_av_pkt_->pts = av_rescale_q(pts_ns, in_time_base, stream->time_base);
            shared_av_pkt_->dts = shared_av_pkt_->pts; 
            shared_av_pkt_->stream_index = 0;
            
            if (pkt.is_keyframe) {
                shared_av_pkt_->flags |= AV_PKT_FLAG_KEY;
            }

            // 取代 av_interleaved_write_frame，实现绕开内存排队的物理极速直通写
            av_write_frame(ctx, shared_av_pkt_);
        }
    } 
    else if (pkt.type == DataType::IMU_RAW) {
        file_imu_bin_.write(reinterpret_cast<const char*>(&real_unix_ns), sizeof(real_unix_ns));
        file_imu_bin_.write(reinterpret_cast<const char*>(&pkt.dev_ts), sizeof(pkt.dev_ts));
        file_imu_bin_.write(reinterpret_cast<const char*>(pkt.payload.data()), pkt.payload.size());
    }

    // 使用 snprintf 替换 C++ 慢速重载运算符，支撑万级 Hz 的变态文本记录
    char log_buf[128];
    int len = std::snprintf(log_buf, sizeof(log_buf), "%llu,%llu,%d,%zu\n",
                            (unsigned long long)pkt.dev_ts,
                            (unsigned long long)pkt.sys_ts,
                            static_cast<int>(pkt.type),
                            pkt.payload.size());
    if (len > 0) {
        file_sync_map_.write(log_buf, len);
    }
}

void StorageWorker::process_loop() {
    DataPacket pkt;
    
    // 初始化为 0，代表尚未收到任何数据
    current_chunk_start_ns_ = 0;

    while (is_running_) {
        // 0 CPU 开销纳秒级瞬时唤醒
        if (data_bridge_.wait_and_pop_packet(pkt)) {
            
            // 【核心逻辑 1】当收到第一帧时，锁定这第一帧的 sys_ts 作为切片基准
            if (current_chunk_start_ns_ == 0) {
                current_chunk_start_ns_ = pkt.sys_ts;
                VIO_LOG_INFO("[Storage Worker] 已锁定首帧系统时间基准: " << current_chunk_start_ns_);
            }

            // 【核心逻辑 2】安全计算时间差，必须先判断大小，杜绝无符号整型下溢 (Underflow)
            if (pkt.sys_ts >= current_chunk_start_ns_) {
                // 如果当前包的系统时间距离基准时间超过了设定的切片时长
                if (pkt.sys_ts - current_chunk_start_ns_ >= CHUNK_DURATION_NS) {
                    // 触发文件轮转。注意：您的 rotate_files 内部会将 current_chunk_start_ns_ 更新为传入的 pkt.sys_ts
                    rotate_files(pkt.sys_ts); 
                }
            } else {
                // 极小概率事件：如果系统时间发生回拨或乱序包到达，忽略本次切片判断直接落盘
                // VIO_LOG_WARN("收到旧时间戳数据包，忽略切片判断");
            }

            // 无论是否切片，数据都正常写入
            write_packet_to_disk(pkt);
            
        } else {
            // 返回 false 代表桥接器已收到了主控的停机指令，安全退出
            break; 
        }
    }
}

void StorageWorker::start() {
    if (is_running_) return;
    is_running_ = true; 
    worker_thread_ = std::thread(&StorageWorker::process_loop, this);
    VIO_LOG_INFO("[Storage Worker] 存储线程已启动.");
}

void StorageWorker::stop() {
    if (is_running_) {
        is_running_ = false;
        
        if (worker_thread_.joinable()) worker_thread_.join();
        
        DataPacket pkt;
        size_t flushed_count = 0;
        
        // 托底保护：排空最后遗留在桥接器内未来得及落盘的尾帧
        while (data_bridge_.try_pop_packet(pkt)) {
            write_packet_to_disk(pkt);
            flushed_count++;
        }
        
        if (flushed_count > 0) {
            VIO_LOG_INFO("[Storage Worker] 排空" << flushed_count << " 个待处理数据包已写入 MP4 文件。");
        }
        
        close_files();
        VIO_LOG_INFO("[Storage Worker] 存储线程已安全退出。");
    }
}

} // namespace vio