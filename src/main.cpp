#include <iostream>
#include <csignal>
#include <chrono>
#include <thread>
#include <atomic>
#include <string>
#include <fstream>
#include <unistd.h> // 提供 getpid()

#include "core/data_bridge.hpp"
#include "core/encode_worker.hpp"   
#include "providers/oak_provider.hpp"
#include "providers/dv89_provider.hpp"
#include "storage/storage_worker.hpp"
#include "utils/logger.hpp"
#include "utils/time_sync.hpp"

namespace vio {
    // ---------------------------------------------------------
    // 全局校时变量：单调硬件时钟与真实 UNIX 时间的纳秒级偏差值
    // ---------------------------------------------------------
    uint64_t g_unix_offset_ns = 0;
}

using namespace vio;

std::atomic<bool> g_is_running{true};
const std::string PID_FILE = "/tmp/vio_logger.pid";

// ---------------------------------------------------------
// 信号拦截：捕获 SIGINT (Ctrl+C) 和 SIGTERM (标准 Kill)
// ---------------------------------------------------------
void signal_handler(int signum) {
    VIO_LOG_WARN("\n[main] 接收信号 " << signum << " ，正在安全关闭...");
    g_is_running = false;
}

// ---------------------------------------------------------
// ✨ 优化3：利用 RAII 自动管理 PID 文件生命周期
// ---------------------------------------------------------
struct PidFileManager {
    PidFileManager() {
        std::ofstream pid_file(PID_FILE);
        if (pid_file.is_open()) {
            pid_file << getpid() << std::endl;
        }
    }
    ~PidFileManager() {
        std::remove(PID_FILE.c_str());
    }
};

int main(int argc, char** argv) {

    // 统一外部输入参数
    const int TARGET_WIDTH  = 640;
    const int TARGET_HEIGHT = 320;
    const int TARGET_FPS    = 30;

    // 1. 注册信号监听 (防强杀保护)
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // 2. 写入 PID 文件 (无论以何种方式 return，析构函数都会自动删文件)
    PidFileManager pid_manager;

    VIO_LOG_INFO("[main] ============================================");
    VIO_LOG_INFO("[main] VIO 守护进程启动 (PID: " << getpid() << ")");
    VIO_LOG_INFO("[main] ============================================");

    // 3. 高精度全局时间基准校准 (Time Calibration)
    uint64_t mono_now = vio::time::get_sys_time_ns();      
    uint64_t unix_now = vio::time::get_unix_time_ns();      
    vio::g_unix_offset_ns = unix_now - mono_now;           

    VIO_LOG_INFO("[main] 执行全局时间基准校准...");
    VIO_LOG_INFO("[main] mono 时间: " << mono_now << " ns");
    VIO_LOG_INFO("[main] unix 时间 : " << unix_now << " ns");
    VIO_LOG_INFO("[main] 时间偏差 : " << vio::g_unix_offset_ns << " ns (Calibrated)");

    // 4. 解析存储路径
    std::string output_dir;
    if (argc > 1) {
        output_dir = argv[1];
        if (!output_dir.empty() && output_dir.back() != '/') {
            output_dir += '/';
        }
    } else {
        uint64_t unix_time = vio::time::get_unix_timestamp_sec();
        output_dir = "/sdcard/data_" + std::to_string(unix_time) + "/";
    }
    VIO_LOG_INFO("[main] 输出文件目录: " << output_dir);

    // 5. 核心组件实例化
    DataBridge data_bridge;
    StorageWorker storage(data_bridge, output_dir);
    
    // 实例化唯一的单管线三路硬编中心
    EncodeWorker shared_hardware_encoder(data_bridge, TARGET_WIDTH, TARGET_HEIGHT, TARGET_FPS);
    
    // 将硬编中心投喂给 OAK 双目和 DV89 采集端
    OakProvider oak(shared_hardware_encoder); 
    Dv89Provider dv89(shared_hardware_encoder, "/dev/dv89_cam");

    // 6. 按照严格依赖顺序初始化 
    if (!storage.initialize()) {
        VIO_LOG_ERROR("[main] 存储模块初始化失败，无法继续。");
        return -1;
    }
    
    if (!shared_hardware_encoder.start()) {
        VIO_LOG_ERROR("[main] 致命错误：共享硬件编码管线拒绝点亮！VE 驱动异常。");
        return -1;
    }
    
    VIO_LOG_INFO("[main] 正在寻检 OAK-D 硬件流，尝试触发二次枚举...");
    bool oak_ok = oak.initialize(TARGET_WIDTH, TARGET_HEIGHT, TARGET_FPS);
    if (oak_ok) {
        VIO_LOG_INFO("[main] 耀宇 Carina A1088 (OAK-D) 闭源底层管线建立成功！");
    } else {
        VIO_LOG_ERROR("[main] 耀宇双目硬件初始化失败，请检查 lib/ 下的驱动包状态或供电。");
    }

    bool dv89_ok = dv89.initialize(TARGET_WIDTH, TARGET_HEIGHT, TARGET_FPS);
    if (dv89_ok) {
        VIO_LOG_INFO("[main] DV89 辅助通道 V4L2 采样链路初始化圆满成功！");
    } else {
        VIO_LOG_ERROR("[main] DV89 辅助通道 V4L2 节点建立失败，请检查 /dev/video_dv89 映射规则。");
    }

    if (!oak_ok && !dv89_ok) {
        VIO_LOG_ERROR("[main] 两个摄像头初始化都失败了！退出。");
        shared_hardware_encoder.stop();
        return -1;
    }

    // 7. 启动工作线程 (先起消费者，后起生产者)
    storage.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    if (oak_ok) oak.start();
    if (dv89_ok) dv89.start();

    VIO_LOG_INFO("[main] 数据采集已启动，按 Ctrl+C 可安全关闭。");

    // 8. 高频 Watchdog 看门狗循环 (秒级响应 Ctrl+C)
    int watchdog_tick = 0;
    while (g_is_running) {
        if (++watchdog_tick >= 20) { // 20 * 100ms = 2秒触发一次打印
            size_t q_size = data_bridge.get_approx_size();
            if (q_size > 100) {
                VIO_LOG_WARN("[main] 队列大小异常 (" << q_size << "). 可能存在 SD 卡 IO 拥塞！");
            }
            watchdog_tick = 0;
        }
        // 100ms 极速轮询，保证按下 Ctrl+C 瞬间就能跳出循环
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // 9. 严格多米诺关机序列 
    VIO_LOG_INFO("[main] 安全关闭序列启动...");
    if (dv89_ok) dv89.stop();  // 这个不会卡死

    VIO_LOG_INFO("[main] 所有采集端已停止，正在关闭共享硬件编码管线...");
    shared_hardware_encoder.stop();

    VIO_LOG_INFO("[main] 正在刷新 DataBridge 队列...");
    data_bridge.stop(); // ⚡【关键】唤醒在 wait_and_pop 里睡大觉的 StorageWorker

    VIO_LOG_INFO("[main] 正在完成 MP4 trailer 并刷新到 SD 卡。请勿断电！");
    storage.stop();
    
    VIO_LOG_INFO("[main] ============================================");
    VIO_LOG_INFO("[main] VIO 守护进程已安全退出。");
    VIO_LOG_INFO("[main] ============================================");

    return 0;
}