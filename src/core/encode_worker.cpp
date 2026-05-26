#include "core/encode_worker.hpp"
#include "utils/logger.hpp"
#include <queue>
#include <cstring>
#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/dma-heap.h>
#include <gst/allocators/gstdmabuf.h>
#include <gst/video/video.h>

namespace vio {

// =================================================================
// 辅助工具：向 O.S 申请独立物理 CMA 内存
// =================================================================
static int alloc_dmabuf(size_t size) {
    int heap_fd = open("/dev/dma_heap/system", O_RDWR | O_CLOEXEC);
    if (heap_fd < 0) return -1; 
    struct dma_heap_allocation_data data = {0};
    data.len = size;
    data.fd_flags = O_RDWR | O_CLOEXEC;
    if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &data) < 0) {
        close(heap_fd); return -1;
    }
    close(heap_fd);
    return data.fd; 
}

// =================================================================
// 高性能 DMA 内存池实现
// =================================================================

namespace {

struct DmaBufferItem {
    int original_fd = -1;
    uint8_t* mapped_ptr = nullptr;
    size_t size = 0;
};

class DmaPool : public std::enable_shared_from_this<DmaPool> {
public:
    static std::shared_ptr<DmaPool> create(int count, size_t size, size_t y_size) {
        return std::shared_ptr<DmaPool>(new DmaPool(count, size, y_size));
    }

    ~DmaPool() {
        for (auto& item : all_items_) {
            if (item.mapped_ptr && item.mapped_ptr != MAP_FAILED) {
                munmap(item.mapped_ptr, item.size);
            }
            if (item.original_fd >= 0) {
                close(item.original_fd);
            }
        }
    }

    DmaBufferItem* acquire() {
        std::unique_lock<std::mutex> lock(mtx_);
        cv_.wait(lock, [this]() { return !free_queue_.empty() || abort_; });
        if (abort_ && free_queue_.empty()) return nullptr;
        
        auto* item = free_queue_.front();
        free_queue_.pop();
        return item;
    }

    void release(DmaBufferItem* item) {
        std::lock_guard<std::mutex> lock(mtx_);
        free_queue_.push(item);
        cv_.notify_one();
    }

    void abort() {
        std::lock_guard<std::mutex> lock(mtx_);
        abort_ = true;
        cv_.notify_all();
    }

private:
    DmaPool(int count, size_t size, size_t y_size) {
        all_items_.reserve(count); // 防止 vector 扩容导致指针失效
        for (int i = 0; i < count; ++i) {
            DmaBufferItem item;
            item.size = size;
            item.original_fd = alloc_dmabuf(size);
            if (item.original_fd >= 0) {
                item.mapped_ptr = static_cast<uint8_t*>(mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, item.original_fd, 0));
                if (item.mapped_ptr != MAP_FAILED) {
                    // 仅在池化初始化时执行一次 memset，将 UV 分量刷为灰色 (128)
                    std::memset(item.mapped_ptr + y_size, 128, size - y_size);
                    all_items_.push_back(item);
                } else {
                    close(item.original_fd);
                }
            }
        }
        // 将成功初始化的 buffer 压入空闲队列
        for (auto& item : all_items_) {
            free_queue_.push(&item);
        }
    }

    std::vector<DmaBufferItem> all_items_;
    std::queue<DmaBufferItem*> free_queue_;
    std::mutex mtx_;
    std::condition_variable cv_;
    bool abort_ = false;
};
}


// 跨线程回收上下文
struct ReclaimContext {
    std::shared_ptr<DmaPool> pool;
    DmaBufferItem* item;
};

// GStreamer Buffer 彻底释放时的回调钩子
static void dma_buffer_destroy_notify(gpointer data) {
    auto* ctx = static_cast<ReclaimContext*>(data);
    if (ctx && ctx->pool && ctx->item) {
        ctx->pool->release(ctx->item); // 归还给内存池
    }
    delete ctx; // 清理上下文
}


// 全局静态芯片 VE 寄存器硬件加锁扣件，强行隔离状态机激活动作窗
static std::mutex g_hardware_ve_activation_mutex;

// ── 1. 左目硬件出口 ──
GstFlowReturn EncodeWorker::on_left_sample(GstAppSink* sink, gpointer user_data) {
    auto* self = static_cast<EncodeWorker*>(user_data);
    GstSample* sample = gst_app_sink_pull_sample(sink);
    if (!sample) return GST_FLOW_OK;
    GstBuffer* buffer = gst_sample_get_buffer(sample);
    if (buffer) {
        uint64_t dev_ts = GST_BUFFER_PTS(buffer);
        constexpr uint64_t INTERVAL = 33333333ULL;
        uint64_t safe_mux_ts = self->left_frame_count_ * INTERVAL;
        self->left_frame_count_++;

        GstMapInfo map;
        if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
            DataPacket out(DataType::OAK_H264_LEFT, safe_mux_ts, dev_ts, map.size);
            out.payload.assign(map.data, map.data + map.size);
            out.is_keyframe = !(GST_BUFFER_FLAGS(buffer) & GST_BUFFER_FLAG_DELTA_UNIT);
            self->get_bridge().push_packet(std::move(out));
            gst_buffer_unmap(buffer, &map);
        }
    }
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

// ── 2. 右目硬件出口 ──
GstFlowReturn EncodeWorker::on_right_sample(GstAppSink* sink, gpointer user_data) {
    auto* self = static_cast<EncodeWorker*>(user_data);
    GstSample* sample = gst_app_sink_pull_sample(sink);
    if (!sample) return GST_FLOW_OK;
    GstBuffer* buffer = gst_sample_get_buffer(sample);
    if (buffer) {
        uint64_t dev_ts = GST_BUFFER_PTS(buffer);
        constexpr uint64_t INTERVAL = 33333333ULL;
        uint64_t safe_mux_ts = self->right_frame_count_ * INTERVAL;
        self->right_frame_count_++;

        GstMapInfo map;
        if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
            DataPacket out(DataType::OAK_H264_RIGHT, safe_mux_ts, dev_ts, map.size);
            out.payload.assign(map.data, map.data + map.size);
            out.is_keyframe = !(GST_BUFFER_FLAGS(buffer) & GST_BUFFER_FLAG_DELTA_UNIT);
            self->get_bridge().push_packet(std::move(out));
            gst_buffer_unmap(buffer, &map);
        }
    }
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

// ── 3. DV89 硬件出口 ──
GstFlowReturn EncodeWorker::on_dv89_sample(GstAppSink* sink, gpointer user_data) {
    auto* self = static_cast<EncodeWorker*>(user_data);
    GstSample* sample = gst_app_sink_pull_sample(sink);
    if (!sample) return GST_FLOW_OK;
    GstBuffer* buffer = gst_sample_get_buffer(sample);
    if (buffer) {
        uint64_t dev_ts = GST_BUFFER_PTS(buffer);
        constexpr uint64_t INTERVAL = 33333333ULL;
        uint64_t safe_mux_ts = self->dv89_frame_count_ * INTERVAL;
        self->dv89_frame_count_++;

        GstMapInfo map;
        if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
            DataPacket out(DataType::DV89_H264, safe_mux_ts, dev_ts, map.size);
            out.payload.assign(map.data, map.data + map.size);
            out.is_keyframe = !(GST_BUFFER_FLAGS(buffer) & GST_BUFFER_FLAG_DELTA_UNIT);
            self->get_bridge().push_packet(std::move(out));
            gst_buffer_unmap(buffer, &map);
        }
    }
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

EncodeWorker::EncodeWorker(DataBridge& bridge, int width, int height, int fps)
    : data_bridge_(bridge), width_(width), height_(height), fps_(fps), is_running_(false) {
}

EncodeWorker::~EncodeWorker() {
    stop();
}

bool EncodeWorker::build_single_pipeline(GstElement*& pipeline, GstElement*& appsrc, GstElement*& appsink, 
                                         const std::string& pipe_name, const std::string& src_name, 
                                         const std::string& sink_name, GstAppSinkCallbacks& cb) { 
    // 使用传入的格式
    GError* error = nullptr;
    
    // 所有上游传入现统一被组装或天生即为 NV12，无需格式参数动态传递
    std::string src_caps = "video/x-raw,format=NV12,width=" + std::to_string(width_) + 
                           ",height=" + std::to_string(height_) + ",framerate=" + std::to_string(fps_) + "/1";

    // 因为输入就是 NV12，直接给 omxh264videoenc！彻底消灭软封包卡点！
    std::string desc = 
        "appsrc name=" + src_name + " is-live=true format=time block=false caps=\"" + src_caps + "\" ! " +
        "omxh264videoenc output-width=" + std::to_string(width_) + 
        " output-height=" + std::to_string(height_) + " periodicity-idr=30 ! " +
        "appsink name=" + sink_name + " emit-signals=true sync=false";
    pipeline = gst_parse_launch(desc.c_str(), &error);
    if (error) {
        VIO_LOG_ERROR("[encode]管线语法故障: " << error->message);
        g_error_free(error); return false;
    }

    appsrc = gst_bin_get_by_name(GST_BIN(pipeline), src_name.c_str());
    appsink = gst_bin_get_by_name(GST_BIN(pipeline), sink_name.c_str());
    g_object_set(G_OBJECT(appsrc), "do-timestamp", FALSE, NULL);
    gst_app_sink_set_callbacks(GST_APP_SINK(appsink), &cb, this, nullptr);

    // ── 串行化加锁进入 PLAYING ──
    {
        std::lock_guard<std::mutex> lock(g_hardware_ve_activation_mutex);
        if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
            VIO_LOG_ERROR("[encode]拒绝为独立工位 [" << pipe_name << "] 释流。");
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return true;
}

bool EncodeWorker::start() {
    if (is_running_) return true;
    gst_init(nullptr, nullptr);
    VIO_LOG_INFO("[encode]正在构建 三路管道 解码器...");

    left_frame_count_ = 0; right_frame_count_ = 0; dv89_frame_count_ = 0;

    GstAppSinkCallbacks cb_l = { nullptr, nullptr, on_left_sample, nullptr };
    GstAppSinkCallbacks cb_r = { nullptr, nullptr, on_right_sample, nullptr };
    GstAppSinkCallbacks cb_d = { nullptr, nullptr, on_dv89_sample, nullptr };

    
    if (!build_single_pipeline(pipeline_l_, appsrc_l_, appsink_l_, "Pipe_L", "src_l", "sink_l", cb_l) ||
        !build_single_pipeline(pipeline_r_, appsrc_r_, appsink_r_, "Pipe_R", "src_r", "sink_r", cb_r) ||
        !build_single_pipeline(pipeline_d_, appsrc_d_, appsink_d_, "Pipe_D", "src_d", "sink_d", cb_d)) {
        return false;
    }

    is_running_ = true;
    left_thread_  = std::thread(&EncodeWorker::left_drive_loop, this);
    right_thread_ = std::thread(&EncodeWorker::right_drive_loop, this);
    dv89_thread_  = std::thread(&EncodeWorker::dv89_drive_loop, this);

    // 专属硬件推流核心排布 (绑核保护)
    cpu_set_t cpuset_l; CPU_ZERO(&cpuset_l); CPU_SET(7, &cpuset_l); pthread_setaffinity_np(left_thread_.native_handle(), sizeof(cpu_set_t), &cpuset_l);
    cpu_set_t cpuset_r; CPU_ZERO(&cpuset_r); CPU_SET(6, &cpuset_r); pthread_setaffinity_np(right_thread_.native_handle(), sizeof(cpu_set_t), &cpuset_r);
    cpu_set_t cpuset_d; CPU_ZERO(&cpuset_d); CPU_SET(5, &cpuset_d); pthread_setaffinity_np(dv89_thread_.native_handle(), sizeof(cpu_set_t), &cpuset_d);

    VIO_LOG_INFO("[encode] 三管道 解码器链路成功构建，驱动线程已启动，绑定于 Core 7/6/5。");
    return true;
}

void EncodeWorker::stop() {
    if (!is_running_) {
        return;
    }
    is_running_ = false;
    left_cv_.notify_all(); right_cv_.notify_all(); dv89_cv_.notify_all();

    if (left_thread_.joinable())  left_thread_.join();
    if (right_thread_.joinable()) right_thread_.join();
    if (dv89_thread_.joinable())  dv89_thread_.join();

    auto clean_pipe = [](GstElement*& pipe, GstElement*& src, GstElement*& sink) {
        if (pipe) {
            gst_element_set_state(pipe, GST_STATE_NULL);
            if (src) {
                gst_object_unref(src);
            }
            if (sink) {
                gst_object_unref(sink);
            }
            gst_object_unref(pipe);
            pipe = nullptr;
            src = nullptr;
            sink = nullptr;
        }
    };
    clean_pipe(pipeline_l_, appsrc_l_, appsink_l_);
    clean_pipe(pipeline_r_, appsrc_r_, appsink_r_);
    clean_pipe(pipeline_d_, appsrc_d_, appsink_d_);

    RawImagePacket dummy;
    while (left_queue_.try_dequeue(dummy)) {}
    while (right_queue_.try_dequeue(dummy)) {}
    while (dv89_queue_.try_dequeue(dummy)) {}
    VIO_LOG_INFO("[encode]三路物理独占硬编管道安全卸载。");
}


// ==============================================================================
// 物理内存零拷贝透传 API
// ==============================================================================

// void EncodeWorker::push_dmabuf_left(uint64_t sys_ts, uint64_t dev_ts, int dmabuf_fd, size_t size) {
//     if (!is_running_) return;
//     RawImagePacket pkt{ChannelType::LEFT_EYE, sys_ts, dev_ts, {}, dmabuf_fd, size};
//     left_queue_.enqueue(std::move(pkt));
//     left_cv_.notify_one(); 
// }

// void EncodeWorker::push_dmabuf_right(uint64_t sys_ts, uint64_t dev_ts, int dmabuf_fd, size_t size) {
//     if (!is_running_) return;
//     RawImagePacket pkt{ChannelType::RIGHT_EYE, sys_ts, dev_ts, {}, dmabuf_fd, size};
//     right_queue_.enqueue(std::move(pkt));
//     right_cv_.notify_one(); 
// }

void EncodeWorker::push_dmabuf_dv89(uint64_t sys_ts, uint64_t dev_ts, int dmabuf_fd, size_t size) {
    if (!is_running_) return;
    RawImagePacket pkt{ChannelType::DV89_AUX, sys_ts, dev_ts, {}, dmabuf_fd, size};
    dv89_queue_.enqueue(std::move(pkt));
    dv89_cv_.notify_one(); 
}

void EncodeWorker::push_raw_left(uint64_t sys_ts, uint64_t dev_ts, const uint8_t* data, size_t size) {
    if (!is_running_) return;
    left_queue_.enqueue(RawImagePacket{ChannelType::LEFT_EYE, sys_ts, dev_ts, std::vector<uint8_t>(data, data + size)});
    left_cv_.notify_one(); 
}

void EncodeWorker::push_raw_right(uint64_t sys_ts, uint64_t dev_ts, const uint8_t* data, size_t size) {
    if (!is_running_) return;
    right_queue_.enqueue(RawImagePacket{ChannelType::RIGHT_EYE, sys_ts, dev_ts, std::vector<uint8_t>(data, data + size)});
    right_cv_.notify_one(); 
}

void EncodeWorker::push_raw_dv89(uint64_t sys_ts, uint64_t dev_ts, const uint8_t* data, size_t size) {
    if (!is_running_) return;
    dv89_queue_.enqueue(RawImagePacket{ChannelType::DV89_AUX, sys_ts, dev_ts, std::vector<uint8_t>(data, data + size)});
    dv89_cv_.notify_one(); 
}



// ==============================================================================
// 消费者推流线程：完成到 GStreamer 的 DMABUF 接入
// ==============================================================================

void EncodeWorker::left_drive_loop() {
    static GstAllocator* dma_allocator = gst_dmabuf_allocator_new(); 
    size_t y_size = width_ * height_; 
    size_t nv12_size = y_size * 3 / 2;
    
    // 初始化池：预申请 4 个 Buffer，足以应对硬编流水线
    auto pool = DmaPool::create(4, nv12_size, y_size);
    
    // 静态 Quark 用于标记回收回调
    static GQuark q_dma_reclaim = g_quark_from_static_string("dma-pool-reclaim");

    while (is_running_) {
        RawImagePacket raw{};
        { 
            std::unique_lock<std::mutex> lk(left_mtx_); 
            left_cv_.wait(lk, [&]{ return !is_running_ || left_queue_.try_dequeue(raw); }); 
        }
        if (!is_running_ && raw.payload.empty() && raw.dmabuf_fd < 0) break;

        GstBuffer* buf = nullptr;

        if (!raw.payload.empty()) {
            // 1. 从内存池极速获取可用的持久化 DMA Buffer
            DmaBufferItem* item = pool->acquire();
            if (!item) continue; // 如果线程正准备退出，可能返回 nullptr

            // 2. 只拷贝 Y 分量 (省去 memset 开销)
            std::memcpy(item->mapped_ptr, raw.payload.data(), y_size);

            // 3. 克隆 FD (保护原始 FD 不被 GStreamer 强制回收)
            int dup_fd = dup(item->original_fd);
            if (dup_fd >= 0) {
                GstMemory* mem = gst_dmabuf_allocator_alloc(dma_allocator, dup_fd, nv12_size);
                if (mem) {
                    buf = gst_buffer_new();
                    gst_buffer_append_memory(buf, mem); 

                    // 4. 挂载生命周期终结器 (Destroy Notify)
                    auto* ctx = new ReclaimContext{pool, item};
                    gst_mini_object_set_qdata(GST_MINI_OBJECT(buf), q_dma_reclaim, ctx, dma_buffer_destroy_notify);
                } else {
                    close(dup_fd); 
                    pool->release(item); // 托底防泄漏
                }
            } else {
                pool->release(item);
            }
        } 
        
        if (buf) {
            GST_BUFFER_PTS(buf) = raw.dev_ts;  
            GST_BUFFER_DTS(buf) = raw.sys_ts;
            gst_app_src_push_buffer(GST_APP_SRC(appsrc_l_), buf);
        }
    }
    
    // 退出时唤醒所有卡在 acquire() 上的等待
    pool->abort(); 
}

void EncodeWorker::right_drive_loop() {
    static GstAllocator* dma_allocator = gst_dmabuf_allocator_new(); 
    size_t y_size = width_ * height_; 
    size_t nv12_size = y_size * 3 / 2;
    
    auto pool = DmaPool::create(4, nv12_size, y_size);
    static GQuark q_dma_reclaim = g_quark_from_static_string("dma-pool-reclaim");

    while (is_running_) {
        RawImagePacket raw{};
        { 
            std::unique_lock<std::mutex> lk(right_mtx_); 
            right_cv_.wait(lk, [&]{ return !is_running_ || right_queue_.try_dequeue(raw); }); 
        }
        if (!is_running_ && raw.payload.empty() && raw.dmabuf_fd < 0) break;

        GstBuffer* buf = nullptr;

        if (!raw.payload.empty()) {
            DmaBufferItem* item = pool->acquire();
            if (!item) continue;

            std::memcpy(item->mapped_ptr, raw.payload.data(), y_size);

            int dup_fd = dup(item->original_fd);
            if (dup_fd >= 0) {
                GstMemory* mem = gst_dmabuf_allocator_alloc(dma_allocator, dup_fd, nv12_size);
                if (mem) {
                    buf = gst_buffer_new(); 
                    gst_buffer_append_memory(buf, mem); 

                    auto* ctx = new ReclaimContext{pool, item};
                    gst_mini_object_set_qdata(GST_MINI_OBJECT(buf), q_dma_reclaim, ctx, dma_buffer_destroy_notify);
                } else {
                    close(dup_fd); 
                    pool->release(item); 
                }
            } else {
                pool->release(item);
            }
        } 
        
        if (buf) {
            GST_BUFFER_PTS(buf) = raw.dev_ts; 
            GST_BUFFER_DTS(buf) = raw.sys_ts;
            gst_app_src_push_buffer(GST_APP_SRC(appsrc_r_), buf);
        }
    }

    pool->abort();
}

void EncodeWorker::dv89_drive_loop() {
    static GstAllocator* dma_allocator = gst_dmabuf_allocator_new(); 
    size_t y_size = width_ * height_; 
    size_t nv12_size = y_size * 3 / 2;
    
    // 初始化池：预申请 4 个 Buffer
    auto pool = DmaPool::create(4, nv12_size, y_size);
    static GQuark q_dma_reclaim = g_quark_from_static_string("dma-pool-reclaim");

    while (is_running_) {
        RawImagePacket raw{};
        { 
            std::unique_lock<std::mutex> lk(dv89_mtx_); 
            dv89_cv_.wait(lk, [&]{ return !is_running_ || dv89_queue_.try_dequeue(raw); }); 
        }
        if (!is_running_ && raw.payload.empty() && raw.dmabuf_fd < 0) break;

        GstBuffer* buf = nullptr;

        // 接收 YUYV 原始数据，在 DMA 缓冲区中进行 YUYV -> NV12 转换
        if (!raw.payload.empty()) {
            DmaBufferItem* item = pool->acquire();
            if (!item) continue;

            uint8_t* yuyv_data = raw.payload.data();
            size_t yuyv_size = raw.payload.size();
            
            // ======= YUYV (4:2:2) -> NV12 (4:2:0) 实时转换 =======
            uint8_t* y_ptr = item->mapped_ptr;
            uint8_t* uv_ptr = item->mapped_ptr + y_size;
            
            if (yuyv_size >= y_size * 2) {
                // 逐行处理 YUYV 转 NV12
                for (int j = 0; j < height_; ++j) {
                    const uint8_t* line_yuyv = yuyv_data + j * width_ * 2;
                    uint8_t* line_y = y_ptr + j * width_;
                    uint8_t* line_uv = uv_ptr + (j / 2) * width_;
                    
                    for (int i = 0; i < width_; i += 2) {
                        // 提取 Y 分量 (每个像素都有)
                        line_y[i] = line_yuyv[i * 2];          // Y0
                        line_y[i + 1] = line_yuyv[i * 2 + 2];  // Y1
                        
                        // 提取 U, V 分量 (4:2:0 垂直下采样，只在偶数行抓取)
                        if (j % 2 == 0) {
                            line_uv[i] = line_yuyv[i * 2 + 1];     // U0
                            line_uv[i + 1] = line_yuyv[i * 2 + 3]; // V0
                        }
                    }
                }
            } else {
                VIO_LOG_WARN("[DV89] 接收到的 YUYV 帧大小异常，跳过本帧！");
                pool->release(item);
                continue;
            }
            // =====================================================

            int dup_fd = dup(item->original_fd);
            if (dup_fd >= 0) {
                GstMemory* mem = gst_dmabuf_allocator_alloc(dma_allocator, dup_fd, nv12_size);
                if (mem) {
                    buf = gst_buffer_new(); 
                    gst_buffer_append_memory(buf, mem); 

                    auto* ctx = new ReclaimContext{pool, item};
                    gst_mini_object_set_qdata(GST_MINI_OBJECT(buf), q_dma_reclaim, ctx, dma_buffer_destroy_notify);
                } else {
                    close(dup_fd); 
                    pool->release(item); 
                }
            } else {
                pool->release(item);
            }
        } 
        
        if (buf) {
            GST_BUFFER_PTS(buf) = raw.dev_ts; 
            GST_BUFFER_DTS(buf) = raw.sys_ts;
            gst_app_src_push_buffer(GST_APP_SRC(appsrc_d_), buf);
        }
    }

    pool->abort();
}

// // DV89 驱动线程与前两路略有不同，因为它直接接收 DMA-BUF FD，无需再进行内存拷贝，完全零拷贝透传给 GStreamer
// void EncodeWorker::dv89_drive_loop() {
//     static GstAllocator* dma_allocator = gst_dmabuf_allocator_new(); 
    
//     // ----------- 新增此行以便使用 gst_buffer_add_video_meta -----------
//     static GstVideoInfo vinfo;
//     gst_video_info_set_format(&vinfo, GST_VIDEO_FORMAT_NV12, width_, height_);
//     // ------------------------------------------------------------------

//     while (is_running_) {
//         RawImagePacket raw{};
//         { std::unique_lock<std::mutex> lk(dv89_mtx_); dv89_cv_.wait(lk, [&]{ return !is_running_ || dv89_queue_.try_dequeue(raw); }); }
//         if (!is_running_ && raw.payload.empty() && raw.dmabuf_fd < 0) break;

//         GstBuffer* buf = nullptr;

//         if (raw.dmabuf_fd >= 0) {
//             int dup_fd = dup(raw.dmabuf_fd); 
//             if (dup_fd >= 0) {
//                 GstMemory* mem = gst_dmabuf_allocator_alloc(dma_allocator, dup_fd, raw.size);
//                 if (mem) {
//                     buf = gst_buffer_new();
//                     gst_buffer_append_memory(buf, mem); 
                    
//                     // --- 最关键的一步：将视频元数据(步长等)附着到裸 Buffer 上！ ---
//                     gst_buffer_add_video_meta(buf, GST_VIDEO_FRAME_FLAG_NONE, 
//                                               GST_VIDEO_FORMAT_NV12, width_, height_);
//                     // ------------------------------------------------------------------
                    
//                 } else close(dup_fd);
//             }
//         }

//         if (buf) {
//             GST_BUFFER_PTS(buf) = raw.dev_ts; 
//             GST_BUFFER_DTS(buf) = raw.sys_ts;
//             gst_app_src_push_buffer(GST_APP_SRC(appsrc_d_), buf);
//         }
//     }
// }

} // namespace vio