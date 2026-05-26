#include <iostream>
#include <vector>
#include <atomic>
#include <unistd.h>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <dirent.h>
#include <cstring>
#include "providers/carina_a1088.h"

std::atomic<bool> g_frame_received{false};

// ---------------------------------------------------------
// 硬件探针：动态扫描 sysfs，匹配你的 UDEV 规则 (0906/03e7)
// ---------------------------------------------------------
bool check_camera_hardware(std::string& current_vid) {
    DIR *dir = opendir("/sys/bus/usb/devices/");
    if (!dir) return false;
    
    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') continue;
        
        std::string vid_path = std::string("/sys/bus/usb/devices/") + entry->d_name + "/idVendor";
        std::ifstream vid_file(vid_path);
        if (vid_file.is_open()) {
            std::string vid;
            vid_file >> vid;
            // 匹配 FUSB300 (Bootloader) 或 MyriadX (运行态)
            if (vid == "0906" || vid == "03e7") {
                current_vid = vid;
                closedir(dir);
                return true;
            }
        }
    }
    closedir(dir);
    return false;
}

// ---------------------------------------------------------
// 打印与回调
// ---------------------------------------------------------
void print_row_hex(const char* data, int width, int row_idx) {
    printf("Row %d (Raw First 32 bytes): ", row_idx);
    for (int i = 0; i < 32 && i < width; ++i) {
        printf("%02x ", (unsigned char)data[i]);
    }
    printf("\n");
}

static void on_imu_data(float *imu, double ts, void* userdata) {
    (void)imu; (void)ts; (void)userdata;
}

static void on_camera_data(char *left, char *right, char *left1, char *right1, double ts, int w, int h, void* userdata) {
    if (g_frame_received.load() || !left) return;
    
    printf("\n[Padding Check] 成功接收到第一帧画面: %dx%d\n", w, h);
    print_row_hex(left, w, 0);
    print_row_hex(left + w, w, 1);
    printf("--------------------------------------------\n");
    
    g_frame_received.store(true);
}

// ---------------------------------------------------------
// 主函数
// ---------------------------------------------------------
int main() {
    
    // ---------------------------------------------------------
    // 【新增】：开局强制将进程的工作目录切换到项目根目录
    // ---------------------------------------------------------
    const char* project_root = "/home/radxa/demo_vio/data_collect";
    if (chdir(project_root) != 0) {
        printf("【警告】强制切换工作目录失败！请检查路径是否存在。\n");
    } else {
        printf("【系统】进程工作目录(CWD)已成功锁定至: %s\n", project_root);
    }
    
    printf("============================================================\n");
    printf("[Carina SDK] 硬件级自适应测试程序 (UDEV 匹配版)\n");
    printf("============================================================\n");

    // 1. 硬件级阻断拦截：如果系统里根本没有设备，直接退出，绝不给 SDK 崩溃的机会
    std::string active_vid;
    if (!check_camera_hardware(active_vid)) {
        printf("\n【严重错误】硬件未连接！\n");
        printf("在系统 /sys/bus/usb/ 中既找不到 0906 也找不到 03e7。\n");
        printf("请检查电源线、USB 接口，或运行 lsusb 确认硬件状态。\n");
        return -1;
    }

    if (active_vid == "0906") {
        printf("[硬件状态] 检测到初始态 (0906)，准备触发二次枚举...\n");
    } else {
        printf("[硬件状态] 检测到运行态 (03e7)，设备已就绪！\n");
    }

    // 2. 配置文件安全加载
    std::string config_path = "./oak_vio_config/custom_config.yaml";
    std::string vocab_path  = "./oak_vio_config/database.bin";

    std::ifstream config_file(config_path);
    if (!config_file.is_open()) {
        printf("[错误] 无法加载 YAML: %s\n", config_path.c_str());
        return -1;
    }
    std::stringstream buffer;
    buffer << config_file.rdbuf();
    std::string config_content = buffer.str();
    config_file.close();

    std::vector<char> mut_config(config_content.begin(), config_content.end());
    mut_config.push_back('\0'); 
    std::vector<char> mut_vocab(vocab_path.begin(), vocab_path.end());
    mut_vocab.push_back('\0');

    // 3. 启动 SDK (传入 -1, -1 让 SDK 依托 UDEV 规则自动游走)
    printf("正在初始化 Carina 底层管线...\n");
    int ret = carina_a1088_init(mut_config.data(), mut_vocab.data(), -1, -1, -1);
    if (ret != 0) {
        printf("[错误] SDK 拒绝初始化，错误码: %d\n", ret);
        return -1;
    }

    // ------ 新增：死等二次枚举完成，防止底层空指针崩溃 ------
    printf("固件已推送，正在阻塞等待设备以 03e7 身份重新上线...\n");
    int boot_wait = 0;
    while (!carina_a1088_is_device_connect() && boot_wait < 50) {
        usleep(100000); // 100ms
        boot_wait++;
    }
    if (!carina_a1088_is_device_connect()) {
        printf("【超时】设备二次枚举失败，未能重新连接！\n");
        return -1;
    }
    printf("设备已完全就绪！正在拉起算法线程...\n");
    // ---------------------------------------------------------

    carina_a1088_start(nullptr, nullptr, on_imu_data, on_camera_data, nullptr, nullptr, nullptr);
    carina_a1088_resume();
    
    printf("指令已下达，正在等待底层驱动吐出数据流 (10秒超时)...\n");
    
    int timeout_ms = 0;
    while(!g_frame_received.load() && timeout_ms < 10000) { 
        usleep(100000); 
        timeout_ms += 100;
        if (timeout_ms % 1000 == 0) {
            printf("等待数据... %d/10 秒\n", timeout_ms / 1000);
        }
    }

    printf("\n清理资源...\n");
    if (g_frame_received.load()) {
        printf(">>> 测试圆满成功！十六进制数据已输出！ <<<\n");
    } else {
        printf("【超时警告】未收到画面。请确保摄像头插在 蓝色 USB 3.0 接口！\n");
    }
    
    carina_a1088_pause();
    carina_a1088_stop();
    carina_a1088_release();
    return 0;
}