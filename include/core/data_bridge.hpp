#pragma once

#include "data_packet.hpp"
// 引入 moodycamel 的阻塞版本无锁队列
#include "blockingconcurrentqueue.h" 
#include <cstddef>
#include <atomic>

namespace vio {

/**
 * @brief 数据桥接类 (DataBridge)
 * 作用：连接数据采集线程与存储/编码线程。
 * 特性：无锁高吞吐、零延迟阻塞唤醒、异步解耦。
 */
class DataBridge {
public:
    DataBridge();
    ~DataBridge();

    DataBridge(const DataBridge&) = delete;
    DataBridge& operator=(const DataBridge&) = delete;

    /**
     * @brief 生产者调用：推入数据，瞬间唤醒休眠中的消费者
     */
    void push_packet(DataPacket&& pkt);

    /**
     * @brief [核心升级] 消费者调用：零 CPU 消耗的阻塞等待
     * @param out_pkt 用于接收弹出的数据包
     * @return true 表示成功拿到数据；false 表示系统正在退出且队列已排空
     */
    bool wait_and_pop_packet(DataPacket& out_pkt);

    /**
     * @brief 保留原有的非阻塞提取，适用于关机扫尾等特殊场景
     */
    bool try_pop_packet(DataPacket& out_pkt);

    /**
     * @brief 安全停机机制，通知消费者停止阻塞并退出
     */
    void stop();

    size_t get_approx_size() const;

private:
    moodycamel::BlockingConcurrentQueue<DataPacket> packet_queue_;
    std::atomic<bool> is_running_{true};
};

} // namespace vio