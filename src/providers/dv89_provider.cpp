#include "providers/dv89_provider.hpp"
#include "utils/logger.hpp"
#include "utils/time_sync.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <cstring>
#include <pthread.h>

namespace vio {

Dv89Provider::Dv89Provider(EncodeWorker& encoder, const std::string& device_path)
    : encoder_(encoder), device_path_(device_path), is_running_(false), v4l2_fd_(-1), width_(640), height_(320), fps_(30) {}

Dv89Provider::~Dv89Provider() { stop(); }

bool Dv89Provider::initialize(int width, int height, int fps) {
    width_ = width;
    height_ = height;
    fps_ = fps;
    return init_v4l2();
}

bool Dv89Provider::init_v4l2() {

    int ret = 0;
    
    v4l2_fd_ = open(device_path_.c_str(), O_RDWR | O_NONBLOCK, 0);
    if (v4l2_fd_ == -1) {
        VIO_LOG_ERROR("[DV89 V4L2]无法打开设备节点: " << device_path_ << ". 请检查映射规则和权限设置。");
        return false;
    }

    // 配置分辨率与格式 (NV12 是全志 VPU 硬编的最佳适配格式)
    struct v4l2_format fmt;
    std::memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = width_;
    fmt.fmt.pix.height = height_;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV; // 后续会把 UV 分量刷成灰色以兼容 NV12 的大小 
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    ret = ioctl(v4l2_fd_, VIDIOC_S_FMT, &fmt);
    if (ret < 0) {
        VIO_LOG_ERROR("[DV89 V4L2]无法设置视频格式。");
        return false;
    }

    // 设定帧率
    struct v4l2_streamparm parm;
    std::memset(&parm, 0, sizeof(parm));
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = fps_;
    ret = ioctl(v4l2_fd_, VIDIOC_S_PARM, &parm);
    if (ret < 0) {
        VIO_LOG_ERROR("[DV89 V4L2]无法设置帧率。");
        return false;
    }

    // 请求 MMAP 缓冲区
    struct v4l2_requestbuffers req;
    std::memset(&req, 0, sizeof(req));
    req.count = V4L2_BUFFER_COUNT;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    ret = ioctl(v4l2_fd_, VIDIOC_REQBUFS, &req);
    if (ret < 0) {
        VIO_LOG_ERROR("[DV89 V4L2]无法请求 MMAP 缓冲区。");
        return false;
    }

    buffers_.resize(req.count);
    for (size_t i = 0; i < req.count; ++i) {
        struct v4l2_buffer buf;
        std::memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        ret = ioctl(v4l2_fd_, VIDIOC_QUERYBUF, &buf);
        if (ret < 0) {
            VIO_LOG_ERROR("[DV89 V4L2]无法查询缓冲区信息。");
            return false;
        }

        buffers_[i].length = buf.length;
        buffers_[i].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, v4l2_fd_, buf.m.offset);
        
        
        if (buffers_[i].start != MAP_FAILED) {
            size_t y_size = width_ * height_;
            // 如果缓冲区大小足够 NV12 格式，则将 UV 分量赋为 128 (消色差/灰色)
            if (buf.length >= y_size * 3 / 2) {
                std::memset(static_cast<uint8_t*>(buffers_[i].start) + y_size, 128, buf.length - y_size);
            }
        }
        // 通过 VIDIOC_EXPBUF 将 V4L2 缓冲区导出为 DMA-BUF FD
        struct v4l2_exportbuffer expbuf;
        std::memset(&expbuf, 0, sizeof(expbuf));
        expbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        expbuf.index = i;
        expbuf.flags = O_CLOEXEC | O_RDWR; 
        ret = ioctl(v4l2_fd_, VIDIOC_EXPBUF, &expbuf);
        if (ret < 0) {
            VIO_LOG_ERROR("[DV89 V4L2]无法导出 DMA-BUF (VIDIOC_EXPBUF)");
            return false;
        }
        buffers_[i].fd = expbuf.fd;  // 保存导出的文件描述符

        // 入队
        ret = ioctl(v4l2_fd_, VIDIOC_QBUF, &buf);
        if (ret < 0) {
            VIO_LOG_ERROR("[DV89 V4L2]无法入队缓冲区。");
            return false;
        }
    }
    return true;
}

void Dv89Provider::capture_loop() {

    while (is_running_) {
        struct v4l2_buffer buf;
        std::memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        
        if (ioctl(v4l2_fd_, VIDIOC_DQBUF, &buf) == 0) {
            uint64_t sys_ts = vio::time::get_unix_time_ns();
            uint64_t dev_ts = static_cast<uint64_t>(buf.timestamp.tv_sec) * 1e9 + buf.timestamp.tv_usec * 1e3;
            
            // ================== 直推 YUYV 原始数据 ==================
            uint8_t* frame_ptr = static_cast<uint8_t*>(buffers_[buf.index].start);
            
            // 直接把 YUYV 原始数据丢给编码器，由其负责 YUYV -> NV12 转换
            encoder_.push_raw_dv89(sys_ts, dev_ts, frame_ptr, buf.bytesused);
            // =========================================================

            // // 直推 DMA-BUF FD 给硬编 Worker，实现真正的零拷贝透传
            // encoder_.push_dmabuf_dv89(sys_ts, dev_ts, buffers_[buf.index].fd, buf.bytesused);
            
            ioctl(v4l2_fd_, VIDIOC_QBUF, &buf);
        }
    }
}

void Dv89Provider::start() {
    
    int ret = 0;
    is_running_ = true;
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    // 启动视频流，进入采集循环
    ret = ioctl(v4l2_fd_, VIDIOC_STREAMON, &type);
    if (ret < 0) {
        VIO_LOG_ERROR("[DV89 V4L2]无法启动视频流。");
        is_running_ = false;
        return;
    }
    // 启动独立线程进行持续采集，确保主线程不受阻塞
    capture_thread_ = std::thread(&Dv89Provider::capture_loop, this);
    
    // 绑核优化，剥离 Core 5 用于采集线程，避免与主线程的调度竞争
    cpu_set_t cpuset; CPU_ZERO(&cpuset); CPU_SET(5, &cpuset);
    // 注意：pthread_setaffinity_np 需要在 capture_thread_ 启动后调用，否则会因为线程未创建而导致调用失败
    pthread_setaffinity_np(capture_thread_.native_handle(), sizeof(cpu_set_t), &cpuset);

    VIO_LOG_INFO("[DV89 V4L2] DV89 辅助通道采集线程已启动，绑定于 Core 5。");

}

void Dv89Provider::stop() {

    int ret = 0;

    if (is_running_) {
        is_running_ = false;
        if (capture_thread_.joinable()) {
            capture_thread_.join();
        }

        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ret = ioctl(v4l2_fd_, VIDIOC_STREAMOFF, &type);
        if (ret < 0) {
            VIO_LOG_ERROR("[DV89 V4L2]无法停止视频流。");
        }

        // 彻底释放内存映射与文件描述符
        for (auto& buf : buffers_) {
            if (buf.start != MAP_FAILED && buf.start != nullptr) {
                munmap(buf.start, buf.length);
            }
            if (buf.fd >= 0) {
                close(buf.fd);  // 关闭这个通过 EXPBUF 申请的 DMA-BUF 文件描述符
                buf.fd = -1;
            }
        }
        
        if (v4l2_fd_ != -1) {
            close(v4l2_fd_);
            v4l2_fd_ = -1;
        }

        VIO_LOG_INFO("[DV89 V4L2] DV89 直通链路资源完全解绑卸载。");
    }
}

} // namespace vio