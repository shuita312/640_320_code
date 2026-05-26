#include "core/data_bridge.hpp"
#include "utils/logger.hpp"
#include <chrono>

namespace vio {

DataBridge::DataBridge() : is_running_(true) {
    VIO_LOG_INFO("DataBridge (Blocking Lock-free Queue) initialized successfully.");
}

DataBridge::~DataBridge() {
    stop(); // 确保在销毁前解除所有的线程阻塞
    
    size_t remain = get_approx_size();
    if (remain > 0) {
        VIO_LOG_WARN("DataBridge destroyed with " << remain << " unprocessed packets dropped!");
    } else {
        VIO_LOG_INFO("DataBridge destroyed cleanly.");
    }
}

void DataBridge::push_packet(DataPacket&& pkt) {
    // 底层直接推入数据，如果消费者正在休眠，操作系统信号量会瞬间唤醒它
    packet_queue_.enqueue(std::move(pkt));
    
    // 容量监控防爆栈机制：
    size_t current_size = packet_queue_.size_approx();
    if (current_size > 500 && current_size % 100 == 0) { 
        VIO_LOG_WARN("DataBridge queue is growing! Current size: " << current_size 
                     << " - Consumer thread might be lagging behind!");
    }
}

bool DataBridge::wait_and_pop_packet(DataPacket& out_pkt) {
    // 带有超时机制的阻塞等待（完美兼顾了瞬间唤醒与安全退出）
    // 1. 如果队列有数据：直接无锁弹出，耗时纳秒级。
    // 2. 如果队列空：线程进入 0 CPU 占用的深度休眠。有数据来时瞬间唤醒。
    // 3. 100ms 超时打断：这并非轮询，而是为了每秒检查 10 次 is_running_ 状态，以防死锁。
    while (is_running_) {
        if (packet_queue_.wait_dequeue_timed(out_pkt, std::chrono::milliseconds(100))) {
            return true;
        }
    }
    
    // 如果 is_running_ 变为 false，退出休眠并尝试排空队列中最后遗留的数据
    return packet_queue_.try_dequeue(out_pkt);
}

bool DataBridge::try_pop_packet(DataPacket& out_pkt) {
    return packet_queue_.try_dequeue(out_pkt);
}

void DataBridge::stop() {
    is_running_ = false;
}

size_t DataBridge::get_approx_size() const {
    return packet_queue_.size_approx();
}

} // namespace vio