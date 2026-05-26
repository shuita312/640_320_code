#include "providers/oak_provider.hpp"
#include "utils/logger.hpp"
#include "utils/time_sync.hpp"
#include <cstring>
#include <fstream>   

namespace vio {

// =================================================================
// 静态回调函数：由 Carina SDK 底层硬件中断触发
// =================================================================

static void on_imu_data(float *imu, double ts, void* userdata) {
    if (!userdata || !imu) return;
    OakProvider* provider = static_cast<OakProvider*>(userdata);
    DataBridge& bridge = provider->get_bridge();

    uint64_t dev_ts_ns = static_cast<uint64_t>(ts * 1e9);
    uint64_t current_sys_ts = vio::time::get_unix_time_ns();

    DataPacket pkt(DataType::IMU_RAW, current_sys_ts, dev_ts_ns, sizeof(ImuRawData));
    pkt.payload.resize(sizeof(ImuRawData));

    ImuRawData* raw = reinterpret_cast<ImuRawData*>(pkt.payload.data());
    raw->accel_x = imu[0];
    raw->accel_y = imu[1];
    raw->accel_z = imu[2];
    raw->gyro_x  = imu[3];
    raw->gyro_y  = imu[4];
    raw->gyro_z  = imu[5];

    bridge.push_packet(std::move(pkt));
}

static void on_camera_data(char *left, char *right, char *left1, char *right1, double ts, int w, int h, void* userdata) {
    (void)left1; (void)right1;
    if (!userdata) return;

    OakProvider* provider = static_cast<OakProvider*>(userdata);
    EncodeWorker& encoder = provider->get_encoder();

    uint64_t dev_ts_ns = static_cast<uint64_t>(ts * 1e9);
    uint64_t current_sys_ts = vio::time::get_unix_time_ns();
    size_t y_size = static_cast<size_t>(w * h);

    // 【关键排查点】：定义 SDK 的真实内存跨距 (Stride)
    // 默认先尝试等于 width (w)。如果重新编译后 OAK 的画面依然有黑白横纹撕裂，
    // 请在此处手动修改 real_stride，常见的对齐值有：224, 256, 352, 384, 512 或 640。
    size_t real_stride = w;  // <-- 如果撕裂未消失，请重点调整这个数值！
    size_t buffer_size = real_stride * h;

    // 极度轻量：仅做一次极短内存拷贝到结构体内，毫不拖泥带水，把所有的组装转换留给工作线程去做
    if (left) {
        encoder.push_raw_left(current_sys_ts, dev_ts_ns, reinterpret_cast<uint8_t*>(left), y_size);
    }
    if (right) {
        encoder.push_raw_right(current_sys_ts, dev_ts_ns, reinterpret_cast<uint8_t*>(right), y_size);
    }
}

// =================================================================
// 类成员函数实现
// =================================================================

// 修正构造函数初始化列表，深度绑定传入的引用类型编码器，彻底扑灭没有匹配函数的 error
OakProvider::OakProvider(EncodeWorker& encoder) 
    : encoder_(encoder)
    , width_(640)       // 默认分辨率，后续会被配置文件覆盖
    , height_(320)      // 默认分辨率，后续会被配置文件覆盖
    , fps_(30)          // 默认帧率，后续会被配置文件覆盖
    , is_running_(false) {
}

OakProvider::~OakProvider() {
    stop();
}

bool OakProvider::initialize(int width, int height, int fps) {
    VIO_LOG_INFO("[Carina A1088]开始初始化 initialize()...");

    // =================================================================
    // 动态解析物理拓扑路径 "2-1" 锁死蓝色 USB 3.1 接口
    // =================================================================
    int bus_num = -1;
    int dev_addr = -1;
    
    std::string bus_path = "/sys/bus/usb/devices/2-1/busnum";
    std::string dev_path = "/sys/bus/usb/devices/2-1/devnum";
    
    std::ifstream bus_file(bus_path);
    std::ifstream dev_file(dev_path);
    
    if (bus_file.is_open() && dev_file.is_open()) {
        bus_file >> bus_num;
        dev_file >> dev_addr;
        if (bus_num == -1 || dev_addr == -1) {
            VIO_LOG_WARN("[Carina A1088] 警告：未能获取正确的物理总线地址，数采系统将以不稳定状态运行，请检查 OAK-D 是否在蓝色接口！");
            return false; // 严禁在路径探测失败的情况下启动 SDK
        }
        VIO_LOG_INFO("[Carina A1088]物理拓扑探测成功！蓝色 USB 3.1 接口当前状态 -> Bus: " << bus_num << ", Device Address: " << dev_addr);
    } else {
        VIO_LOG_WARN("[Carina A1088]无法通过 sysfs 探测物理路径 2-1，将尝试降级使用全总线扫描...");
    }
    bus_file.close();
    dev_file.close();
    
    // =================================================================
    // 加载相机配置文件
    // =================================================================
    
    std::string config_path = "./oak_vio_config/custom_config.yaml";
    std::string vocab_path = "./oak_vio_config/database.bin";
    
    std::ifstream config_file(config_path);
    if (!config_file.is_open()) {
        VIO_LOG_ERROR("[Carina A1088]无法打开相机配置文件，请检查路径是否正确: " << config_path);
        return false;
    }
    std::stringstream buffer;
    buffer << config_file.rdbuf();
    std::string config_content = buffer.str(); 
    config_file.close();

    // =================================================================
    // 注入动态精确定位参数，阻断总线串扰
    // =================================================================
    int ret = carina_a1088_init(
        const_cast<char*>(config_content.c_str()), 
        const_cast<char*>(vocab_path.c_str()),
        -1,  // file_descriptor，留空由 SDK 内部处理
        bus_num, // 优先锁定物理路径 2-1
        dev_addr // 优先锁定物理路径 2-1
    );

    if (ret != 0) {
        VIO_LOG_ERROR("[Carina A1088]初始化失败，错误代码: " << ret);
        return false;
    }
    
    if (!carina_a1088_is_device_connect()) {
        VIO_LOG_ERROR("[Carina A1088]设备未物理连接或未被识别！");
        return false;
    }

    // 与配置文件对齐参数
    this->width_ = width;
    this->height_ = height;
    this->fps_ = fps;
    this->frame_size_ = width * height ;
    
    VIO_LOG_INFO("[Carina A1088] 初始化成功，等待启动...");
    VIO_LOG_INFO("[Carina A1088] SN: " << carina_a1088_get_sn());
    return true;
}

void OakProvider::start() {
    if (is_running_) return;

    VIO_LOG_INFO("[Carina A1088] 正在启动数据流 start()...");

    int ret = carina_a1088_start(
        nullptr,         
        nullptr,         
        on_imu_data,     
        on_camera_data,  
        nullptr,         
        nullptr,         
        this             // 传递 this 指针桥接 C 回调环境
    );

    if (ret == 0) {
        carina_a1088_resume();
        is_running_ = true;
        VIO_LOG_INFO("[Carina A1088] 数据流启动成功！");
    } else {
        VIO_LOG_ERROR("[Carina A1088] 启动数据流失败，错误代码: " << ret);
    }
}

void OakProvider::stop() {
    if (is_running_) {
        VIO_LOG_INFO("[Carina A1088] 正在停止数据流...");
        carina_a1088_pause();
        carina_a1088_stop();
        carina_a1088_release();
        
        is_running_ = false;
        VIO_LOG_INFO("[Carina A1088] 数据流已停止.");

    }
}

} // namespace vio